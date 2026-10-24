// The two front microphones (ES7210), 16 kHz. Records a question and decides when it's over:
// the first 250 ms measure the room's noise floor, then speech is anything clearly above it, and the
// question ends after 1.1 s of quiet (same rules as the browser test page).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static const char *TAG = "voice";
volatile float voice_level = 0;
volatile bool voice_log = false;

#define CHUNK 512                         // samples per read (32 ms)
#define CALIBRATE_MS 250
#define END_SILENCE_MS 1100
#define NO_SPEECH_MS 4500
#define MIC_GAIN_DB 30.0f

static esp_codec_dev_handle_t mic;
static int16_t *pcm;                      // mono, PSRAM
static int16_t stereo[CHUNK * 2];

void voice_init(void)
{
    bsp_audio_codec_speaker_init();        // brings up the shared I2S bus the mic codec uses
    mic = bsp_audio_codec_microphone_init();
    if (!mic) { ESP_LOGE(TAG, "microphone codec init failed"); return; }
    esp_codec_dev_sample_info_t fs = {.sample_rate = SAMPLE_RATE, .channel = 2, .bits_per_sample = 16};
    if (esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "mic open failed"); mic = NULL; return; }
    esp_codec_dev_set_in_gain(mic, MIC_GAIN_DB);
    pcm = heap_caps_malloc(SAMPLE_RATE * MAX_LISTEN_MS / 1000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "microphones ready");
}

size_t voice_listen(const int16_t **out, bool *speech)
{
    *out = pcm; *speech = false;
    if (!mic || !pcm) return 0;
    for (int i = 0; i < 4; i++) esp_codec_dev_read(mic, stereo, sizeof stereo);   // stale DMA data
    size_t n = 0, max = SAMPLE_RATE * MAX_LISTEN_MS / 1000;
    float floor = 0; int floor_n = 0;
    int ms = 0, quiet_since = 0;
    while (n + CHUNK <= max) {
        if (esp_codec_dev_read(mic, stereo, sizeof stereo) != ESP_CODEC_DEV_OK) break;
        double ss = 0;
        for (int i = 0; i < CHUNK; i++) {
            int16_t v = (int16_t)(((int)stereo[2 * i] + stereo[2 * i + 1]) / 2);
            pcm[n + i] = v;
            ss += (double)v * v;
        }
        n += CHUNK;
        ms += CHUNK * 1000 / SAMPLE_RATE;
        float rms = sqrtf(ss / CHUNK) / 32768.0f;
        voice_level = fminf(1, rms * 14);
        if (ms <= CALIBRATE_MS) { floor = (floor * floor_n + rms) / (floor_n + 1); floor_n++; continue; }
        float thresh = fmaxf(0.012f, floor * 2.5f);
        if (rms > thresh) { *speech = true; quiet_since = ms; }
        if (voice_log) printf("mic rms %.4f floor %.4f %s\n", rms, floor, rms > thresh ? "SPEECH" : "");
        if (*speech && ms - quiet_since > END_SILENCE_MS) break;
        if (!*speech && ms > NO_SPEECH_MS) break;
    }
    voice_level = 0;
    ESP_LOGI(TAG, "recorded %.1f s, speech=%d, noise floor %.4f", n / (float)SAMPLE_RATE, *speech, floor);
    return n;
}
