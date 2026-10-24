// Talking 8-ball firmware: shared declarations.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLE_RATE 16000
#define MAX_LISTEN_MS 9000

// --- events into the app task ---------------------------------------------------------
typedef enum { EV_SHAKE, EV_TAP, EV_REVEAL_TEXT, EV_LISTEN } app_event_kind_t;
typedef struct {
    app_event_kind_t kind;
    char text[96];               // EV_REVEAL_TEXT
} app_event_t;
void app_post(const app_event_t *ev);
void app_post_kind(app_event_kind_t kind);

// --- scene (scene.c): the screen ------------------------------------------------------------
void scene_init(void);
void scene_listen(void);              // rings and glow; level comes from voice_level
void scene_think(void);               // the die tumbles deep in the liquid
void scene_reveal(const char *answer);// render the triangle with this text and let it rise
bool scene_busy(void);                // the triangle is on its way up or showing
void scene_set_tilt(float x, float y);// -1..1 from the motion sensor
void scene_agitate(float amount);     // shaking stirs the liquid
void scene_wake(void);                // screen back on after idling
void scene_snapshot_to_console(void); // debug: dump the screen as base64 RGB565
void scene_fps_log(bool on);          // debug: log fps and redraw cost every 2 s

// --- motion (motion.c) -----------------------------------------------------------------------
void motion_init(void);
extern volatile bool motion_log;

// --- voice (voice.c) ---------------------------------------------------------------------
void voice_init(void);
// Records until the speaker stops (or MAX_LISTEN_MS). Returns samples recorded; *speech tells whether
// anyone spoke. The buffer stays valid until the next call.
size_t voice_listen(const int16_t **pcm, bool *speech);
extern volatile float voice_level;     // 0..1 while listening, for the rings
extern volatile bool voice_log;

// --- network (net.c) -----------------------------------------------------------------------
typedef struct {
    char ssid[33], pass[65], server[96], token[64], lang[4];
} app_config_t;
extern app_config_t app_cfg;
void config_load(void);
void config_save(void);
void net_init(void);
bool net_connected(void);
const char *net_status(void);
// Sends the recording to the server. Returns true with the answer and language filled in.
bool net_ask(const int16_t *pcm, size_t samples, char *answer, size_t answer_len, char *lang, size_t lang_len);

// --- console (console.c): setup and debugging over USB ---------------------------------------
void console_init(void);

// --- offline answers (answers.c) --------------------------------------------------------------
const char *answers_pick(const char *lang);
