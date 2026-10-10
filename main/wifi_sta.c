/*
 * wifi_sta - WiFi station bring-up with unlimited, paced reconnects.
 *
 * Design notes, lifted from the sibling esp32-httpd project because they were
 * learned the hard way there:
 *   - Nothing here blocks. The caller starts the HTTP server and this module in
 *     either order; the link comes up whenever it comes up.
 *   - Reconnect uses a one-shot esp_timer rather than calling esp_wifi_connect()
 *     straight from the event handler, so an unreachable AP cannot make the
 *     event task spin.
 *   - Shared state is a bool and a uint32, so the event task and the HTTP task
 *     can read it without a lock.
 */
#include "wifi_sta.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "sofa-wifi";

/* Read from the event task, written by the event task's callbacks, consumed by
 * the HTTP and console tasks. Both are word-sized, so no lock is needed. */
static volatile bool s_connected;
static volatile uint32_t s_ip_addr;   /* IPv4, network byte order */

static esp_timer_handle_t s_retry_timer;
static uint32_t s_failed_attempts;    /* consecutive failures, for log wording only */
static wifi_state_cb_t s_state_cb;

/* Map the Kconfig choice onto the driver enum. An unset choice member expands
 * to 0 in #if, so exactly one branch is taken. */
#if CONFIG_SOFA_WIFI_AUTH_WPA_PSK
#define SOFA_AUTH_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_SOFA_WIFI_AUTH_WPA2_PSK
#define SOFA_AUTH_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_SOFA_WIFI_AUTH_WPA3_PSK
#define SOFA_AUTH_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_SOFA_WIFI_AUTH_WPA2_WPA3_PSK
#define SOFA_AUTH_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#else
#define SOFA_AUTH_THRESHOLD WIFI_AUTH_OPEN
#endif

void wifi_sta_set_state_cb(wifi_state_cb_t cb)
{
    s_state_cb = cb;
}

static void notify(bool connected)
{
    if (s_state_cb != NULL) {
        s_state_cb(connected);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "reconnecting (attempt %" PRIu32 ")", s_failed_attempts + 1);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* Most likely the driver is not in a state that allows a connect yet;
         * the next disconnect event re-arms the timer. */
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    esp_err_t err = esp_timer_start_once(s_retry_timer,
                                        (uint64_t)CONFIG_SOFA_WIFI_RETRY_DELAY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot arm reconnect timer: %s", esp_err_to_name(err));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;   /* registered for WIFI_EVENT only */

    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "station started, joining \"%s\"", CONFIG_SOFA_WIFI_SSID);
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)data;
        ESP_LOGI(TAG, "associated with \"%.*s\" on channel %u",
                 e->ssid_len, (const char *)e->ssid, (unsigned)e->channel);
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_failed_attempts++;
        ESP_LOGW(TAG, "disconnected (reason %d), %" PRIu32
                      " consecutive failure(s) -- retrying in %d ms",
                 e->reason, s_failed_attempts, CONFIG_SOFA_WIFI_RETRY_DELAY_MS);
        notify(false);
        schedule_reconnect();
        break;
    }

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;   /* registered for IP_EVENT_STA_GOT_IP only */
    (void)id;

    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    s_ip_addr = e->ip_info.ip.addr;
    s_connected = true;
    s_failed_attempts = 0;
    notify(true);

    char ip[16];
    wifi_sta_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "got IP %s (netmask " IPSTR ", gateway " IPSTR ")",
             ip, IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
    ESP_LOGI(TAG, "sofa control is at http://%s/", ip);
}

esp_err_t wifi_sta_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta() failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TCP/IP stack up; sockets can now be created");
    return ESP_OK;
}

esp_err_t wifi_sta_start(void)
{
    if (strlen(CONFIG_SOFA_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "CONFIG_SOFA_WIFI_SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    /* Keep the association in RAM: the default WIFI_STORAGE_FLASH rewrites NVS
     * on every (re)connect, which is pointless wear for a board that is meant
     * to reconnect unattended. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_ip_event, NULL, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_timer_cb,
        .name = "wifi-retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    wifi_config_t sta_cfg = { 0 };
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid),
             "%s", CONFIG_SOFA_WIFI_SSID);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password),
             "%s", CONFIG_SOFA_WIFI_PASSWORD);
    sta_cfg.sta.threshold.authmode = (strlen(CONFIG_SOFA_WIFI_PASSWORD) == 0)
                                         ? WIFI_AUTH_OPEN
                                         : SOFA_AUTH_THRESHOLD;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Modem sleep costs ~100-300 ms of latency on the first packet after an
     * idle period, which a browser notices. This board is mains-powered and
     * also has a BLE link to hold up, so keep radio duty cycle steady. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "station up; association is asynchronous and retried forever");
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

void wifi_sta_ip_str(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    const esp_ip4_addr_t addr = { .addr = s_ip_addr };
    if (s_connected) {
        esp_ip4addr_ntoa(&addr, out, (int)out_len);
    } else {
        snprintf(out, out_len, "0.0.0.0");
    }
}

void wifi_sta_ssid_str(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%s", (const char *)ap.ssid);
}

int8_t wifi_sta_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
