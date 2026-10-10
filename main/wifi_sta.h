/*
 * wifi_sta - WiFi station bring-up with unlimited reconnects.
 *
 * The sofa link is the point of this firmware and works with no network at all,
 * so this module is strictly optional: if CONFIG_SOFA_WIFI_SSID is empty,
 * app_main never calls it and the console remains the only way in.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Bring up the TCP/IP stack and the default station netif.
 *
 * MUST run before anything that creates a socket, including
 * http_server_start(). lwIP's socket layer reaches its TCP/IP thread through a
 * mailbox that only esp_netif_init() creates, so calling httpd_start() first
 * aborts with
 *   "assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)"
 *
 * Synchronous, and harmless to call before an SSID is configured.
 */
esp_err_t wifi_sta_init(void);

/**
 * @brief Start the station and begin associating.
 *
 * Requires a prior wifi_sta_init(). Registers the event handlers, arms the
 * reconnect timer and returns as soon as the station has started; the
 * association itself completes asynchronously and is retried forever.
 *
 * @return
 *  - ESP_OK                : station started, connection in progress
 *  - ESP_ERR_INVALID_STATE : CONFIG_SOFA_WIFI_SSID is empty
 *  - other                 : propagated from the WiFi driver
 */
esp_err_t wifi_sta_start(void);

/** @brief True while a DHCP lease is held. Cleared as soon as the link drops. */
bool wifi_sta_is_connected(void);

/** @brief Copy the current IPv4 address as dotted text; writes "0.0.0.0" when unconnected. */
void wifi_sta_ip_str(char *out, size_t out_len);

/** @brief SSID of the network we are joined to; writes "" when not associated. */
void wifi_sta_ssid_str(char *out, size_t out_len);

/** @brief RF signal strength in dBm, or 0 when not associated. */
int8_t wifi_sta_rssi(void);

/*
 * Called from the WiFi event task whenever the link comes up or goes down.
 * The status LED shows "everything works", which needs the sofa link too, so
 * the fan-in lives in main.c next to the BLE one rather than here.
 */
typedef void (*wifi_state_cb_t)(bool connected);
void wifi_sta_set_state_cb(wifi_state_cb_t cb);
