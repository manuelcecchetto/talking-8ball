// QMI8658 motion sensor: tilt for the liquid, shaking to ask.
// A shake is several sharp changes of acceleration within half a second; a single bump is not.
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
#define JERK_TRIGGER 9000.0f      // summed |Δa| in mg over the window
#define COOLDOWN_MS 2500

static void motion_task(void *arg)
{
    qmi8658_dev_t *dev = arg;
    float jerk[WINDOW] = {0}, sum = 0, lx = 0, ly = 0, px = 0, py = 0, pz = 1000;
    int idx = 0, samples = 0;
    int64_t last_shake = 0, last_log = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000 / HZ));
        float x, y, z;
        if (qmi8658_read_accel(dev, &x, &y, &z) != ESP_OK) continue;
        if (samples++ == 0) { px = lx = x; py = ly = y; pz = z; }   // no jump from made-up values
        float j = fabsf(x - px) + fabsf(y - py) + fabsf(z - pz);
        px = x; py = y; pz = z;
        sum += j - jerk[idx];
        jerk[idx] = j;
        idx = (idx + 1) % WINDOW;
        lx += (x - lx) * 0.1f; ly += (y - ly) * 0.1f;
        scene_set_tilt(-ly / 700.0f, lx / 700.0f);
        if (sum > 2500) scene_agitate((sum - 2500) / 2500.0f);   // moving the ball stirs the liquid
        int64_t now = esp_timer_get_time() / 1000;
        if (sum > JERK_TRIGGER && now - last_shake > COOLDOWN_MS && samples > WINDOW + HZ) {   // start-up jolt left the window
            last_shake = now;
            ESP_LOGI(TAG, "shake (jerk %.0f mg)", sum);
            app_post_kind(EV_SHAKE);
        }
        if (motion_log && now - last_log > 200) {
            last_log = now;
            printf("imu x %6.0f y %6.0f z %6.0f  jerk %6.0f\n", x, y, z, sum);
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
