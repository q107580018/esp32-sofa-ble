/*
 * http_server - control the sofa from a browser on the same LAN.
 *
 * Routes:
 *   GET  /                     dashboard (main/index.html, linked via EMBED_FILES)
 *   GET  /api/status           live JSON: WiFi, BLE link, movement state, devices
 *   POST /api/move?seat=&dir=  press a seat: seat=1..4|all, dir=up|down
 *   POST /api/stop[?seat=]     stop; without a seat, stop whatever is held
 *   POST /api/hold             keep-alive while a jog button is held down
 *   POST /api/mode?value=      oneshot | jog
 *   POST /api/dir?value=       forward | reverse
 *   POST /api/raw              body is hex text, e.g. "fa 55"
 *   POST /api/scan             rescan / reconnect
 *   GET  /favicon.ico          204, so browsers stop logging 404s
 *
 * Why POST for everything that moves furniture: a GET is something a browser
 * will happily issue on its own - prefetch, a link preview, a reload - and this
 * is a sofa, not an idempotent resource. GET /api/status is the only read.
 *
 * Every handler runs on the single httpd task, so the shared JSON buffer below
 * is safe; the sofa_* layer underneath is thread-safe on its own terms.
 */
#include "http_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "sofa_ble.h"
#include "sofa_control.h"
#include "sofa_proto.h"
#include "wifi_sta.h"

static const char *TAG = "sofa-http";

/* main/index.html, linked in by EMBED_FILES in main/CMakeLists.txt. The element
 * type is const uint8_t; the server wants a char pointer. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* Worst case is the device table at its fullest (16 entries, ~105 bytes each)
 * plus every counter at its widest. The httpd task stack is raised to 12 KB in
 * http_server_start() to make room for this plus snprintf. */
#define STATUS_JSON_MAX 3072
#define RAW_BODY_MAX    64
#define QUERY_MAX       128

/* ---------------------------------------------------------------- helpers */

/* The SSID and every device name are whatever the world calls itself, so they
 * get escaped rather than pasted into JSON. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t w = 0;
    if (out_len == 0) {
        return;
    }
    for (const char *p = in; *p != '\0' && w + 7 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 0x20) {
            w += (size_t)snprintf(out + w, out_len - w, "\\u%04x", c);
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

/* Maps a sofa_* error onto an HTTP status and a sentence a person can act on. */
static esp_err_t send_result(httpd_req_t *req, const char *action, esp_err_t err)
{
    char body[192];
    int n;

    if (err == ESP_OK) {
        n = snprintf(body, sizeof(body), "{\"ok\":true,\"action\":\"%s\"}", action);
    } else {
        const char *status;
        const char *why;
        switch (err) {
        case ESP_ERR_INVALID_STATE:
            status = "409 Conflict";
            why = "the sofa link is not ready";
            break;
        case ESP_ERR_NOT_SUPPORTED:
            status = "409 Conflict";
            why = "in one-shot mode a seat stops on the same control that started it";
            break;
        case ESP_ERR_TIMEOUT:
            status = "503 Service Unavailable";
            why = "the send queue is full, try again";
            break;
        case ESP_ERR_NOT_FOUND:
            status = "409 Conflict";
            why = "no seat has been commanded yet, so there is nothing to stop";
            break;
        case ESP_ERR_INVALID_ARG:
            status = "400 Bad Request";
            why = "bad request";
            break;
        default:
            status = "500 Internal Server Error";
            why = "the request could not be sent";
            break;
        }
        httpd_resp_set_status(req, status);
        n = snprintf(body, sizeof(body),
                     "{\"ok\":false,\"action\":\"%s\",\"error\":\"%s\"}", action, why);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n < 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    return httpd_resp_send(req, body, n);
}

/* Reads one query parameter into `out`. Returns false when it is absent. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[QUERY_MAX];

    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

/* --------------------------------------------------------------- handlers */

/* GET / */
static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (ssize_t)(index_html_end - index_html_start));
}

/* GET /api/status */
static esp_err_t get_status(httpd_req_t *req)
{
    static char json[STATUS_JSON_MAX];

    sofa_ble_info_t info;
    sofa_ble_get_info(&info);

    sofa_status_t reported;
    bool have_reported = sofa_ble_last_status(&reported);

    char ssid_raw[33] = { 0 };
    char ssid[80];
    char ip[16];
    char mac[18];
    wifi_sta_ssid_str(ssid_raw, sizeof(ssid_raw));
    json_escape(ssid_raw, ssid, sizeof(ssid));
    wifi_sta_ip_str(ip, sizeof(ip));

    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    esp_chip_info_t chip;
    esp_chip_info(&chip);

#if CONFIG_SOFA_ENABLE_MDNS
    const char *mdns_name = CONFIG_SOFA_MDNS_HOSTNAME ".local";
#else
    const char *mdns_name = "";
#endif

    /* The sofa's own view, when it has told us. "null" rather than a guess. */
    char mode_json[24];
    if (have_reported && reported.has_mode) {
        snprintf(mode_json, sizeof(mode_json), "\"%s\"",
                 reported.mode == 1 ? "oneshot" : "jog");
    } else {
        snprintf(mode_json, sizeof(mode_json), "null");
    }
    char dir_json[24];
    if (have_reported && reported.has_dir) {
        snprintf(dir_json, sizeof(dir_json), "\"%s\"",
                 reported.dir == 1 ? "forward" : "reverse");
    } else {
        snprintf(dir_json, sizeof(dir_json), "null");
    }

    int n = snprintf(json, sizeof(json),
        "{"
          "\"uptime_s\":%" PRId64 ","
          "\"heap_free\":%" PRIu32 ","
          "\"reset_reason\":\"%s\","
          "\"idf\":\"%s\","
          "\"target\":\"%s\","
          "\"cores\":%d,"
          "\"mac\":\"%s\","
          "\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi_dbm\":%d},"
          "\"server\":{\"port\":%d,\"mdns\":\"%s\"},"
          "\"link\":{"
            "\"state\":\"%s\",\"ready\":%s,\"peer\":\"%s\",\"rssi_dbm\":%d,"
            "\"service_start\":%u,\"service_end\":%u,"
            "\"write_handle\":%u,\"notify_handle\":%u,\"cccd_handle\":%u,\"mtu\":%u,"
            "\"tx_sent\":%" PRIu32 ",\"tx_dropped\":%" PRIu32 ","
            "\"notify_count\":%" PRIu32 ",\"motor_stop_count\":%" PRIu32 ","
            "\"last_motor\":%u"
          "},"
          "\"movement\":{"
            "\"mode\":\"%s\",\"direction\":\"%s\","
            "\"held\":%s,\"held_button\":\"%s\","
            "\"reported_mode\":%s,\"reported_direction\":%s"
          "},"
          "\"devices\":[",
        esp_timer_get_time() / 1000000,
        esp_get_free_heap_size(),
        reset_reason_str(esp_reset_reason()),
        esp_get_idf_version(),
        CONFIG_IDF_TARGET,
        chip.cores,
        mac,
        wifi_sta_is_connected() ? "true" : "false",
        ssid, ip, wifi_sta_rssi(),
        CONFIG_SOFA_HTTP_PORT, mdns_name,
        sofa_state_name(info.state), info.connected ? "true" : "false",
        info.peer_addr, info.rssi,
        info.svc_start, info.svc_end,
        info.chr_write_val, info.chr_notify_val, info.cccd_handle, info.mtu,
        info.tx_sent, info.tx_dropped, info.notify_count, info.motor_stop_count,
        info.last_motor,
        sofa_control_one_shot() ? "oneshot" : "jog",
        sofa_control_forward() ? "forward" : "reverse",
        sofa_control_is_held() ? "true" : "false",
        sofa_button_name(sofa_control_held_button()),
        mode_json, dir_json);

    if (n < 0 || n >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "status buffer too small (needed %d, have %d)", n, (int)sizeof(json));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status buffer too small");
    }

    /* The device table is appended with a loop so the escape helper can be used
     * on names without building the whole thing in one snprintf. */
    sofa_seen_info_t seen[SOFA_SEEN_MAX];
    size_t count = sofa_ble_seen(seen, SOFA_SEEN_MAX);

    for (size_t i = 0; i < count; i++) {
        char name[80];
        json_escape(seen[i].name, name, sizeof(name));
        int w = snprintf(json + n, sizeof(json) - (size_t)n,
                         "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,"
                         "\"matched\":%s,\"seen\":%" PRIu32 "}",
                         i ? "," : "", seen[i].addr, name, seen[i].rssi,
                         seen[i].matched ? "true" : "false", seen[i].seen_count);
        if (w < 0 || w >= (int)(sizeof(json) - (size_t)n)) {
            /* Keep the JSON valid by closing the array instead of truncating
             * mid-object; the client sees fewer devices, not a parse error. */
            ESP_LOGW(TAG, "device list truncated at %u entries", (unsigned)i);
            break;
        }
        n += w;
    }

    int w = snprintf(json + n, sizeof(json) - (size_t)n, "]}");
    if (w < 0 || w >= (int)(sizeof(json) - (size_t)n)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status buffer too small");
    }
    n += w;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, n);
}

/* POST /api/move?seat=<1..4|all>&dir=<up|down> */
static esp_err_t post_move(httpd_req_t *req)
{
    char seat[16];
    char dir[16];
    sofa_button_t up, down;

    if (!query_param(req, "seat", seat, sizeof(seat)) ||
        !sofa_target_buttons(seat, &up, &down)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "expected seat=1|2|3|4|all");
    }
    if (!query_param(req, "dir", dir, sizeof(dir)) ||
        (strcasecmp(dir, "up") != 0 && strcasecmp(dir, "down") != 0)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected dir=up|down");
    }

    /*
     * Arm the hold watchdog *before* moving. A browser that goes away mid-press
     * must not leave the sofa running: in jog mode the firmware is the thing
     * repeating the frame, so it stops itself if /api/hold stops arriving.
     */
    sofa_control_hold_watchdog(CONFIG_SOFA_HOLD_TIMEOUT_MS);

    esp_err_t err = sofa_control_move(strcasecmp(dir, "up") == 0 ? up : down);
    return send_result(req, "move", err);
}

/* POST /api/stop[?seat=<1..4|all>] */
static esp_err_t post_stop(httpd_req_t *req)
{
    char seat[16];
    sofa_button_t up, down;
    esp_err_t err;

    if (query_param(req, "seat", seat, sizeof(seat))) {
        if (!sofa_target_buttons(seat, &up, &down)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "expected seat=1|2|3|4|all");
        }
        err = sofa_control_stop_target(up, down);
    } else {
        err = sofa_control_stop();
    }

    return send_result(req, "stop", err);
}

/* POST /api/hold -- keep-alive while a jog button is held down */
static esp_err_t post_hold(httpd_req_t *req)
{
    sofa_control_keep_alive();
    return send_result(req, "hold", ESP_OK);
}

/* POST /api/mode?value=<oneshot|jog> */
static esp_err_t post_mode(httpd_req_t *req)
{
    char value[16];

    if (!query_param(req, "value", value, sizeof(value))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected value=oneshot|jog");
    }

    bool one_shot;
    if (strcasecmp(value, "oneshot") == 0 || strcasecmp(value, "one-shot") == 0) {
        one_shot = true;
    } else if (strcasecmp(value, "jog") == 0) {
        one_shot = false;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected value=oneshot|jog");
    }

    return send_result(req, "mode", sofa_control_set_one_shot(one_shot));
}

/* POST /api/dir?value=<forward|reverse> */
static esp_err_t post_dir(httpd_req_t *req)
{
    char value[16];

    if (!query_param(req, "value", value, sizeof(value))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected value=forward|reverse");
    }

    bool forward;
    if (strcasecmp(value, "forward") == 0 || strcasecmp(value, "fwd") == 0) {
        forward = true;
    } else if (strcasecmp(value, "reverse") == 0 || strcasecmp(value, "rev") == 0) {
        forward = false;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected value=forward|reverse");
    }

    return send_result(req, "dir", sofa_control_set_forward(forward));
}

/* POST /api/raw -- hex text in the body */
static esp_err_t post_raw(httpd_req_t *req)
{
    char body[RAW_BODY_MAX];
    uint8_t bytes[SOFA_TX_FRAME_MAX];
    size_t len = 0;
    int hi = -1;

    if (req->content_len == 0) {
        return httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "empty body");
    }
    if (req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
    }

    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, NULL);
        }
        return ESP_FAIL;   /* socket error: let the server tear the connection down */
    }
    body[received] = '\0';

    for (const char *p = body; *p != '\0'; p++) {
        char c = *p;
        if (c == ' ' || c == ',' || c == '-' || c == ':' || c == '\n' || c == '\r') {
            continue;
        }
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else if (c == 'x' || c == 'X') {
            continue;   /* tolerate "0x" */
        } else {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body is not hex");
        }

        if (hi < 0) {
            hi = v;
        } else {
            if (len >= SOFA_TX_FRAME_MAX) {
                return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE,
                                           "more than 20 bytes");
            }
            bytes[len++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }

    if (hi >= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "odd number of hex digits");
    }
    if (len == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no bytes in body");
    }

    return send_result(req, "raw", sofa_control_send_raw(bytes, len));
}

/* POST /api/scan */
static esp_err_t post_scan(httpd_req_t *req)
{
    sofa_ble_scan_restart();
    return send_result(req, "scan", ESP_OK);
}

/* GET /favicon.ico -- browsers ask unprompted; answer quietly. */
static esp_err_t get_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/*
 * 404 handler. Registered instead of a wildcard URI.
 *
 * The header warns that req->uri, req->method and req->content_len are NOT
 * guaranteed valid here (the request may only be partially parsed), so this
 * must not read req->uri -- only emit a canned body.
 */
static esp_err_t on_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"not found\"}", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ start */

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_SOFA_HTTP_PORT;
    /* Recycle the least-recently-used socket once all slots are taken, or a
     * browser holding keep-alive connections can lock new requests out. */
    config.lru_purge_enable = true;
    /* Default is 8 and there are ten routes below. */
    config.max_uri_handlers = 12;
    /* Default is 4 KB; /api/status formats a 3 KB JSON body on this stack. */
    config.stack_size = 12288;

    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = get_index   },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = get_status  },
        { .uri = "/api/move",    .method = HTTP_POST, .handler = post_move   },
        { .uri = "/api/stop",    .method = HTTP_POST, .handler = post_stop   },
        { .uri = "/api/hold",    .method = HTTP_POST, .handler = post_hold   },
        { .uri = "/api/mode",    .method = HTTP_POST, .handler = post_mode   },
        { .uri = "/api/dir",     .method = HTTP_POST, .handler = post_dir    },
        { .uri = "/api/raw",     .method = HTTP_POST, .handler = post_raw    },
        { .uri = "/api/scan",    .method = HTTP_POST, .handler = post_scan   },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = get_favicon },
    };

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found));

    ESP_LOGI(TAG, "listening on port %d (%u routes)", config.server_port,
             (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
