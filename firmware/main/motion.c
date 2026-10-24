// QMI8658 motion sensor: tilt for the liquid, shaking to ask.
// A shake is strong acceleration that keeps up: at least HOT_SLICES of the last ten 100 ms slices
// peak over HOT_MG beyond gravity. Measured on the ball: shakes give 4-10 such slices a second;
// picking it up, turning it, walking with it or putting it down never gave more than 1.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "qmi8658.h"
#include "app.h"

static const char *TAG = "motion";
volatile bool motion_log = false;

#define HZ 100
#define WINDOW 50                 // samples (0.5 s)
#define UP_SIGN 1.0f              // the sensor's axes on the screen: flip if the answer comes out upside down
#define SLICE_MS 100
#define HOT_MG 1500.0f            // a slice is hot when the acceleration beyond gravity passes this
#define HOT_SLICES 4              // hot slices in the last 10 (one second) that make a shake
#define COOLDOWN_MS 2500

static void motion_task(void *arg)
{
    qmi8658_dev_t *dev = arg;
    float jerk[WINDOW] = {0}, sum = 0, lx = 0, ly = 0, px = 0, py = 0, pz = 1000;
    float gx = 0, gy = 0, gz = 1000;               // gravity estimate
    float peak = 0;                                // in the current slice
    uint16_t hot = 0;                              // the last 10 slices, one bit each
    int idx = 0, samples = 0;
    int64_t last_shake = 0, slice_end = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000 / HZ));
        float x, y, z;
        if (qmi8658_read_accel(dev, &x, &y, &z) != ESP_OK) continue;
        if (samples++ == 0) { px = lx = gx = x; py = ly = gy = y; pz = gz = z; }   // no jump from made-up values
        float j = fabsf(x - px) + fabsf(y - py) + fabsf(z - pz);
        px = x; py = y; pz = z;
        sum += j - jerk[idx];
        jerk[idx] = j;
        idx = (idx + 1) % WINDOW;
        lx += (x - lx) * 0.2f; ly += (y - ly) * 0.2f;   // ~50 ms: the answer turns with the ball
        scene_set_tilt(-ly / 700.0f, lx / 700.0f);
        scene_set_up(UP_SIGN * ly, -UP_SIGN * lx);   // at rest the sensor reads gravity's opposite: up
        if (sum > 2500) scene_agitate((sum - 2500) / 2500.0f);   // moving the ball stirs the liquid
        int64_t now = esp_timer_get_time() / 1000;

        gx += (x - gx) * 0.02f; gy += (y - gy) * 0.02f; gz += (z - gz) * 0.02f;
        float dx = x - gx, dy = y - gy, dz = z - gz, m = sqrtf(dx * dx + dy * dy + dz * dz);
        if (m > peak) peak = m;
        if (now < slice_end) continue;
        slice_end = now + SLICE_MS;
        hot = ((hot << 1) | (peak > HOT_MG)) & 0x3FF;
        int n = __builtin_popcount(hot);
        if (motion_log) printf("imu %6lld ms  peak %5.0f mg  hot %d  up %4.0f deg\n", now, peak, n,   // console: imu on
                               atan2f(UP_SIGN * ly, UP_SIGN * lx) * 57.3f);
        peak = 0;
        if (n >= HOT_SLICES && now - last_shake > COOLDOWN_MS && samples > WINDOW + HZ) {   // start-up jolt left
            last_shake = now;
            hot = 0;
            ESP_LOGI(TAG, "shake (%d of the last 10 slices over %.1f g)", n, HOT_MG / 1000);
            app_post_kind(EV_SHAKE);
        }
    }
}

void motion_init(void)
{
    qmi8658_dev_t *dev = calloc(1, sizeof *dev);
    if (qmi8658_init(dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 not found: no shake detection (tap the screen instead)");
        return;
    }
    qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_250HZ);
    qmi8658_enable_accel(dev, true);
    qmi8658_set_accel_unit_mps2(dev, false);   // milli-g
    xTaskCreate(motion_task, "motion", 4096, dev, 4, NULL);
}
