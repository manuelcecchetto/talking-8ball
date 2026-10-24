// Talking 8-ball: shake it (or tap the screen), ask out loud, and the answer rises from the liquid.
// The question goes to the server on the PC (server/server.py); without it, or if nobody spoke, the
// ball answers by itself like the original.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static const char *TAG = "8ball";
static QueueHandle_t queue;

void app_post(const app_event_t *ev) { if (queue) xQueueSend(queue, ev, 0); }
void app_post_kind(app_event_kind_t kind) { app_event_t ev = {.kind = kind}; app_post(&ev); }

static void ask(void)
{
    scene_listen();
    const int16_t *pcm;
    bool speech;
    size_t n = voice_listen(&pcm, &speech);
    scene_think();
    int64_t t0 = esp_timer_get_time();
    char answer[128] = "", lang[8] = "";
    bool online = speech && net_ask(pcm, n, answer, sizeof answer, lang, sizeof lang);
    if (online) {
        if (lang[0] && strcmp(lang, app_cfg.lang) != 0 && strlen(lang) < sizeof app_cfg.lang) {
            strlcpy(app_cfg.lang, lang, sizeof app_cfg.lang);
            config_save();
        }
    } else {
        strlcpy(answer, answers_pick(app_cfg.lang), sizeof answer);
    }
    int64_t waited = (esp_timer_get_time() - t0) / 1000;
    if (waited < 1200) vTaskDelay(pdMS_TO_TICKS(1200 - waited));   // let the liquid churn a moment
    ESP_LOGI(TAG, "%s answer: %s", online ? "server" : "offline", answer);
    scene_reveal(answer);
}

static void mem(const char *stage)
{
    ESP_LOGI(TAG, "%-8s internal free %u KB (largest DMA block %u KB), PSRAM free %u KB", stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    queue = xQueueCreate(4, sizeof(app_event_t));
    console_init();
    config_load();
    bsp_i2c_init();
    mem("start");
    net_init();          // first: Wi-Fi needs internal RAM, the screen and mics can live without it
    mem("wifi");
    voice_init();
    mem("voice");
    scene_init();
    mem("scene");
    motion_init();
    ESP_LOGI(TAG, "ready: shake the ball or tap the screen");

    app_event_t ev;
    for (;;) {
        if (xQueueReceive(queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        scene_wake();
        if (scene_busy()) continue;                  // the die is already on its way up
        if (ev.kind == EV_REVEAL_TEXT) { scene_think(); vTaskDelay(pdMS_TO_TICKS(1200)); scene_reveal(ev.text); }
        else ask();
        xQueueReset(queue);                          // shakes during the answer don't queue up questions
    }
}
