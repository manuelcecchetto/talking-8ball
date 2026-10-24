// Settings (NVS), Wi-Fi, finding the PC (mDNS _8ball._tcp, else the saved address) and asking it.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app.h"

static const char *TAG = "net";
app_config_t app_cfg;
static EventGroupHandle_t events;
#define CONNECTED BIT0
static char found_url[96];                 // from mDNS, preferred over the saved address
static char status[96] = "starting";

// --- settings -------------------------------------------------------------------------------
static void get_str(nvs_handle_t h, const char *key, char *out, size_t n)
{
    size_t len = n;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = 0;
}

void config_load(void)
{
    memset(&app_cfg, 0, sizeof app_cfg);
    nvs_handle_t h;
    if (nvs_open("8ball", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", app_cfg.ssid, sizeof app_cfg.ssid);
        get_str(h, "pass", app_cfg.pass, sizeof app_cfg.pass);
        get_str(h, "server", app_cfg.server, sizeof app_cfg.server);
        get_str(h, "token", app_cfg.token, sizeof app_cfg.token);
        get_str(h, "lang", app_cfg.lang, sizeof app_cfg.lang);
        nvs_close(h);
    }
    if (!app_cfg.lang[0]) strcpy(app_cfg.lang, "it");
}

void config_save(void)
{
    nvs_handle_t h;
    if (nvs_open("8ball", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", app_cfg.ssid);
    nvs_set_str(h, "pass", app_cfg.pass);
    nvs_set_str(h, "server", app_cfg.server);
    nvs_set_str(h, "token", app_cfg.token);
    nvs_set_str(h, "lang", app_cfg.lang);
    nvs_commit(h);
    nvs_close(h);
}

// --- Wi-Fi ----------------------------------------------------------------------------------
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(events, CONNECTED);
        snprintf(status, sizeof status, "wifi: disconnected (reason %d), retrying", d->reason);
        ESP_LOGW(TAG, "%s", status);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(status, sizeof status, "wifi: connected to %s as " IPSTR, app_cfg.ssid, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "%s", status);
        xEventGroupSetBits(events, CONNECTED);
    }
}

void net_init(void)
{
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    mdns_init();
    mdns_hostname_set("talking-8ball");
    if (!app_cfg.ssid[0]) {
        snprintf(status, sizeof status, "wifi: not configured (use the console: set ssid ...)");
        ESP_LOGW(TAG, "%s", status);
        return;
    }
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, app_cfg.ssid, strnlen(app_cfg.ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, app_cfg.pass, strnlen(app_cfg.pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = app_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_start();
    snprintf(status, sizeof status, "wifi: connecting to %s", app_cfg.ssid);
}

bool net_connected(void) { return events && (xEventGroupGetBits(events) & CONNECTED); }

const char *net_status(void)
{
    static char s[256];
    snprintf(s, sizeof s, "%s | server %s%s", status, found_url[0] ? found_url : app_cfg.server[0] ? app_cfg.server : "(none)",
             found_url[0] ? " (found by mDNS)" : "");
    return s;
}

static void discover(void)
{
    mdns_result_t *r = NULL;
    if (mdns_query_ptr("_8ball", "_tcp", 1500, 4, &r) != ESP_OK || !r) { found_url[0] = 0; return; }
    for (mdns_result_t *x = r; x; x = x->next)
        for (mdns_ip_addr_t *a = x->addr; a; a = a->next)
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(found_url, sizeof found_url, "http://" IPSTR ":%d", IP2STR(&a->addr.u_addr.ip4), x->port);
                ESP_LOGI(TAG, "server found by mDNS: %s", found_url);
                mdns_query_results_free(r);
                return;
            }
    mdns_query_results_free(r);
}

// --- asking ---------------------------------------------------------------------------------
static bool post(const char *base, const int16_t *pcm, size_t samples, char *answer, size_t alen, char *lang, size_t llen)
{
    char url[128];
    snprintf(url, sizeof url, "%s/ask", base);
    esp_http_client_config_t hc = {.url = url, .method = HTTP_METHOD_POST, .timeout_ms = 30000};
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return false;
    char auth[96];
    snprintf(auth, sizeof auth, "Bearer %s", app_cfg.token);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "audio/wav");

    uint32_t data_len = samples * 2;
    uint8_t h[44];
    uint32_t v;
    memcpy(h, "RIFF", 4); v = 36 + data_len; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); uint16_t s = 1; memcpy(h + 20, &s, 2); memcpy(h + 22, &s, 2);
    v = SAMPLE_RATE; memcpy(h + 24, &v, 4); v = SAMPLE_RATE * 2; memcpy(h + 28, &v, 4);
    s = 2; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data_len, 4);

    bool ok = false;
    if (esp_http_client_open(c, sizeof h + data_len) == ESP_OK) {
        int w = esp_http_client_write(c, (const char *)h, sizeof h);
        for (size_t off = 0; w >= 0 && off < data_len; off += 8192) {
            size_t n = data_len - off < 8192 ? data_len - off : 8192;
            w = esp_http_client_write(c, (const char *)pcm + off, n);
        }
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        char body[1024] = {0};
        int got = 0, r;
        while (got < (int)sizeof body - 1 && (r = esp_http_client_read(c, body + got, sizeof body - 1 - got)) > 0) got += r;
        if (code == 200) {
            cJSON *j = cJSON_Parse(body);
            cJSON *a = cJSON_GetObjectItem(j, "answer"), *l = cJSON_GetObjectItem(j, "language");
            if (cJSON_IsString(a) && a->valuestring[0]) {
                strlcpy(answer, a->valuestring, alen);
                if (cJSON_IsString(l)) strlcpy(lang, l->valuestring, llen);
                ok = true;
            }
            cJSON_Delete(j);
        } else {
            ESP_LOGW(TAG, "server answered %d: %s", code, body);
        }
    } else {
        ESP_LOGW(TAG, "cannot reach %s", url);
    }
    esp_http_client_cleanup(c);
    return ok;
}

bool net_ask(const int16_t *pcm, size_t samples, char *answer, size_t alen, char *lang, size_t llen)
{
    if (!net_connected()) { ESP_LOGW(TAG, "no Wi-Fi: answering offline"); return false; }
    if (!found_url[0]) discover();
    if (found_url[0] && post(found_url, pcm, samples, answer, alen, lang, llen)) return true;
    if (app_cfg.server[0] && strcmp(app_cfg.server, found_url) != 0 && post(app_cfg.server, pcm, samples, answer, alen, lang, llen)) return true;
    found_url[0] = 0;                      // look again next time
    return false;
}
