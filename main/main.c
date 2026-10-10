/*
 * esp32-sofa-ble - drive a BLE "Sofa_wwt_*" recliner from an ESP32, over the
 * serial console or a web page on the LAN.
 *
 * The protocol was recovered from the vendor's Android app; see README.md for
 * what was observed and what is still assumed.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_SOFA_ENABLE_MDNS
#include "mdns.h"
#endif

#include "http_server.h"
#include "sofa_ble.h"
#include "sofa_console.h"
#include "sofa_control.h"
#include "status_led.h"
#include "wifi_sta.h"

static const char *TAG = "app";

/*
 * The status LED answers one question - "can I use this yet" - and that means
 * both links: the sofa has to be ready, and if WiFi is configured the station
 * has to hold a lease. Only app_main is told about both, so the fan-in is here
 * rather than in either module.
 *
 * Both callbacks run on different tasks; each writes its own flag and then
 * recomputes. status_led_set_ready() is latched and mutex-protected, so the
 * worst case is one redundant call.
 */
static volatile bool s_ble_ok;
static volatile bool s_wifi_ok = true;   /* stays true while WiFi is unused */

static void refresh_led(void)
{
    status_led_set_ready(s_ble_ok && s_wifi_ok);
}

static void on_ble_state(sofa_state_t state)
{
    s_ble_ok = (state == SOFA_STATE_READY);
    sofa_control_on_link_state(state);
    refresh_led();
}

static void on_wifi_state(bool connected)
{
    s_wifi_ok = connected;
    refresh_led();
}

#if CONFIG_SOFA_ENABLE_MDNS
static void start_mdns(void)
{
    /* Requires esp_netif_init(), which wifi_sta_init() has already done. */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        /* Not fatal: the control page is reachable by IP either way. */
        ESP_LOGW(TAG, "mdns_init failed (%s) -- continuing without mDNS", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_SOFA_MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("esp32-sofa control"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", CONFIG_SOFA_HTTP_PORT, NULL, 0));

    ESP_LOGI(TAG, "control page: http://%s.local/ (answers once the station has an IP)",
             CONFIG_SOFA_MDNS_HOSTNAME);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "=== esp32-sofa-ble starting ===");
    ESP_LOGI(TAG, "ESP-IDF %s on %s", esp_get_idf_version(), CONFIG_IDF_TARGET);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs re-initialising (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* LED first: with no host attached it is the only output there is, and an
     * unconfigured or mis-pinned one should be obvious from the first second. */
    ESP_ERROR_CHECK(status_led_init());

    sofa_control_init();
    sofa_ble_set_state_cb(on_ble_state);

    /*
     * Boot order matters here, and it is not the order the names suggest:
     *   1. wifi_sta_init()      TCP/IP stack up -- lwIP's socket layer needs it
     *   2. http_server_start()  bind the port, before any link exists (fine)
     *   3. wifi_sta_start()     associate; asynchronous, retried forever
     *
     * Binding before an IP exists works. Creating a socket before
     * esp_netif_init() does not: it aborts at runtime with
     * "assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)", which
     * compiles and links perfectly happily.
     *
     * With no SSID there is no web page at all, and the board is BLE-only.
     */
    const bool wifi_configured = strlen(CONFIG_SOFA_WIFI_SSID) > 0;
    if (wifi_configured) {
        wifi_sta_set_state_cb(on_wifi_state);
        s_wifi_ok = false;
        ESP_ERROR_CHECK(wifi_sta_init());
        ESP_ERROR_CHECK(http_server_start());
        ESP_ERROR_CHECK(wifi_sta_start());
    } else {
        ESP_LOGW(TAG, "no WiFi SSID configured -- the sofa is reachable only over BLE");
        ESP_LOGW(TAG, "  idf.py menuconfig -> 'esp32-sofa-ble configuration' -> 'WiFi SSID'");
    }

    ESP_ERROR_CHECK(sofa_ble_start());

#if CONFIG_SOFA_ENABLE_MDNS
    if (wifi_configured) {
        start_mdns();
    }
#endif

    ESP_LOGI(TAG, "up: name prefix \"%s\", target %s", CONFIG_SOFA_DEVICE_NAME_PREFIX,
             CONFIG_SOFA_TARGET_ADDR);

    ESP_ERROR_CHECK(sofa_console_start());
}
