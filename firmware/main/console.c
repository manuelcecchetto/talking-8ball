// Line commands over the USB cable: setup (Wi-Fi, server, token) and debugging.
//   status | set ssid|pass|server|token|lang <value> | reboot | ask | say <text> | snap | fps on|off | imu on|off | mic on|off
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "app.h"

static void handle(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0; else arg = "";
    if (!strcmp(line, "status")) {
        printf("OK %s | token %s | lang %s | psram free %u KB\n", net_status(), app_cfg.token[0] ? "set" : "missing",
               app_cfg.lang, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    } else if (!strcmp(line, "set")) {
        char *val = strchr(arg, ' ');
        if (val) *val++ = 0; else val = "";
        struct { const char *k; char *dst; size_t n; } f[] = {
            {"ssid", app_cfg.ssid, sizeof app_cfg.ssid}, {"pass", app_cfg.pass, sizeof app_cfg.pass},
            {"server", app_cfg.server, sizeof app_cfg.server}, {"token", app_cfg.token, sizeof app_cfg.token},
            {"lang", app_cfg.lang, sizeof app_cfg.lang}};
        for (size_t i = 0; i < sizeof f / sizeof *f; i++)
            if (!strcmp(arg, f[i].k)) {
                strlcpy(f[i].dst, val, f[i].n);
                config_save();
                printf("OK %s saved (reboot to apply Wi-Fi changes)\n", arg);
                return;
            }
        printf("ERR unknown setting %s\n", arg);
    } else if (!strcmp(line, "reboot")) {
        printf("OK rebooting\n"); fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (!strcmp(line, "ask")) {
        app_post_kind(EV_LISTEN); printf("OK listening\n");
    } else if (!strcmp(line, "say")) {
        app_event_t ev = {.kind = EV_REVEAL_TEXT};
        strlcpy(ev.text, arg, sizeof ev.text);
        app_post(&ev); printf("OK\n");
    } else if (!strcmp(line, "snap")) {
        scene_snapshot_to_console();
    } else if (!strcmp(line, "imu")) {
        motion_log = !strcmp(arg, "on"); printf("OK\n");
    } else if (!strcmp(line, "fps")) {
        scene_fps_log(!strcmp(arg, "on")); printf("OK\n");
    } else if (!strcmp(line, "mic")) {
        voice_log = !strcmp(arg, "on"); printf("OK\n");
    } else if (line[0]) {
        printf("ERR commands: status, set ssid|pass|server|token|lang <value>, reboot, ask, say <text>, snap, fps on|off, imu on|off, mic on|off\n");
    }
    fflush(stdout);
}

static void console_task(void *arg)
{
    static char line[256];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') { line[n] = 0; handle(line); n = 0; }
        else if (n < sizeof line - 1) line[n++] = c;
    }
}

void console_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();       // printf now goes through the same driver
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);
}
