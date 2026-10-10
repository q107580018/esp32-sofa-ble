/*
 * http_server - the HTTP control surface: routes, JSON API, embedded dashboard.
 */
#pragma once

#include "esp_err.h"

/**
 * @brief Start the HTTP server on CONFIG_SOFA_HTTP_PORT.
 *
 * Binds to all interfaces, so this may be called before the station has an IP;
 * the server simply becomes reachable once a lease arrives.
 *
 * It does NOT follow that it may be called before the TCP/IP stack is up:
 * httpd_start() creates a socket, and lwIP's socket layer talks to its TCP/IP
 * thread through a mailbox that esp_netif_init() has to create first. Call
 * wifi_sta_init() before this, or httpd_start() aborts with
 * "assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)".
 *
 * The handle is kept internally -- there is exactly one server per boot.
 *
 * @return ESP_OK on success, otherwise the error from httpd_start().
 */
esp_err_t http_server_start(void);
