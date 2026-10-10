/*
 * sofa_ble - NimBLE central for the sofa.
 *
 * Protocol facts (UUIDs, frame bytes, write type) come from the vendor's
 * Android app, read rather than guessed; README.md has the frame table and the
 * list of what is still assumed.
 *
 * Two threads touch this module:
 *   - the NimBLE host task, where every GAP/GATT callback runs, and
 *   - whoever calls sofa_ble_send()/sofa_ble_get_info() (the console task).
 *
 * The GATT client API is only safe to call from the first of those, so commands
 * are handed over through a FreeRTOS queue plus a ble_npl_event that runs on the
 * host task's own event queue. Everything the other task may read is a plain
 * value guarded by s_lock.
 */
#include "sofa_ble.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

/* Provided by the NimBLE component's store/config; the shipped blecent example
 * forward-declares it the same way rather than pulling in its private header. */
void ble_store_config_init(void);

static const char *TAG = "sofa-ble";

/* Same three UUIDs the app hardcodes and then re-discovers by substring match. */
#define SOFA_SERVICE_UUID_STR "0000ae30-0000-1000-8000-00805f9b34fb"
#define SOFA_WRITE_UUID_STR   "0000ae01-0000-1000-8000-00805f9b34fb"
#define SOFA_NOTIFY_UUID_STR  "0000ae02-0000-1000-8000-00805f9b34fb"

/* ...and their 16-bit halves, for comparing against what is actually on air. */
#define SOFA_SERVICE_UUID16 0xae30
#define SOFA_WRITE_UUID16   0xae01
#define SOFA_NOTIFY_UUID16  0xae02

#define SOFA_CCCD_UUID16 0x2902

#define SOFA_TX_QUEUE_LEN 16

/* How long to wait before re-posting the TX drain after the controller ran out
 * of buffers. Small enough to be invisible, large enough to let the last frame
 * leave the radio. */
#define SOFA_TX_RETRY_US (20 * 1000)

typedef struct {
    uint8_t len;
    uint8_t data[SOFA_TX_FRAME_MAX];
} sofa_frame_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    sofa_state_t state;
    bool connected;

    uint16_t conn_handle;
    char peer_addr[SOFA_ADDR_STR_LEN];
    int8_t peer_rssi;
    uint8_t peer_addr_type;

    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t chr_write_val;
    uint16_t chr_notify_val;
    uint16_t cccd_handle;
    uint16_t mtu;

    bool have_svc;
    bool have_write_chr;
    bool have_notify_chr;
    bool tried_indicate;

    uint32_t notify_count;
    uint32_t motor_stop_count;
    uint8_t last_motor;
    uint32_t tx_sent;
    uint32_t tx_dropped;

    /* Last whole decodable status frame, so callers can show what the sofa
     * itself reported rather than what we last commanded. */
    bool have_status;
    sofa_status_t last_status;

    sofa_state_cb_t state_cb;
    sofa_status_cb_t status_cb;

    sofa_seen_info_t seen[SOFA_SEEN_MAX];
    uint8_t seen_type[SOFA_SEEN_MAX];
    size_t seen_count;
} s;

typedef enum {
    CTL_NONE = 0,
    CTL_RESTART_SCAN,
    CTL_CONNECT,
} sofa_ctl_kind_t;

static ble_uuid_any_t s_uuid_svc;
static ble_uuid_any_t s_uuid_write;
static ble_uuid_any_t s_uuid_notify;

static QueueHandle_t s_txq;
static struct ble_npl_event s_tx_ev;
static struct ble_npl_event s_ctl_ev;
static sofa_ctl_kind_t s_ctl_kind;
static ble_addr_t s_ctl_addr;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_tx_retry_timer;

static void start_scan(void);
static void connect_to(const ble_addr_t *addr, int8_t rssi);
static void schedule_retry(void);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------------ helpers */

const char *sofa_state_name(sofa_state_t state)
{
    switch (state) {
    case SOFA_STATE_BOOT:        return "boot";
    case SOFA_STATE_SCANNING:    return "scanning";
    case SOFA_STATE_CONNECTING:  return "connecting";
    case SOFA_STATE_DISCOVERING: return "discovering";
    case SOFA_STATE_READY:       return "ready";
    case SOFA_STATE_RETRY_WAIT:  return "retry-wait";
    default:                     return "?";
    }
}

static void addr_to_str(const uint8_t val[6], char dst[SOFA_ADDR_STR_LEN])
{
    /* The on-air byte order is little endian, so print it back to front. */
    snprintf(dst, SOFA_ADDR_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             val[5], val[4], val[3], val[2], val[1], val[0]);
}

static bool addr_from_str(const char *str, uint8_t out[6])
{
    unsigned v[6];
    if (str == NULL ||
        sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) {
            return false;
        }
        out[5 - i] = (uint8_t)v[i];
    }
    return true;
}

/*
 * ble_uuid_cmp() refuses to compare across widths (it returns the type
 * difference immediately), and what comes back from discovery depends on how
 * the peripheral spelled the UUID in its attribute table. Our three UUIDs are
 * Bluetooth SIG base UUIDs, so ble_uuid_from_str() already collapses them to
 * 16 bits - but a peripheral that wrote out the long form is still returned
 * long, so accept both.
 */
static bool uuid_matches(const ble_uuid_t *found, const ble_uuid_t *want_any,
                        uint16_t want16)
{
    if (found == NULL) {
        return false;
    }
    if (found->type == want_any->type) {
        return ble_uuid_cmp(found, want_any) == 0;
    }
    if (found->type == BLE_UUID_TYPE_16) {
        return BLE_UUID16(found)->value == want16;
    }
    return false;
}

static void set_state(sofa_state_t state)
{
    sofa_state_cb_t cb;

    portENTER_CRITICAL(&s_lock);
    bool changed = (s.state != state);
    s.state = state;
    cb = s.state_cb;
    portEXIT_CRITICAL(&s_lock);

    if (changed) {
        ESP_LOGI(TAG, "state -> %s", sofa_state_name(state));
        if (cb != NULL) {
            cb(state);
        }
    }
}

void sofa_ble_set_state_cb(sofa_state_cb_t cb)
{
    portENTER_CRITICAL(&s_lock);
    s.state_cb = cb;
    portEXIT_CRITICAL(&s_lock);
}

void sofa_ble_set_status_cb(sofa_status_cb_t cb)
{
    portENTER_CRITICAL(&s_lock);
    s.status_cb = cb;
    portEXIT_CRITICAL(&s_lock);
}

sofa_state_t sofa_ble_state(void)
{
    portENTER_CRITICAL(&s_lock);
    sofa_state_t state = s.state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

bool sofa_ble_ready(void)
{
    return sofa_ble_state() == SOFA_STATE_READY;
}

void sofa_ble_get_info(sofa_ble_info_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    out->state = s.state;
    out->connected = s.connected;
    memcpy(out->peer_addr, s.peer_addr, sizeof(out->peer_addr));
    out->rssi = s.peer_rssi;
    out->svc_start = s.svc_start;
    out->svc_end = s.svc_end;
    out->chr_write_val = s.chr_write_val;
    out->chr_notify_val = s.chr_notify_val;
    out->cccd_handle = s.cccd_handle;
    out->mtu = s.mtu;
    out->notify_count = s.notify_count;
    out->motor_stop_count = s.motor_stop_count;
    out->last_motor = s.last_motor;
    out->tx_sent = s.tx_sent;
    out->tx_dropped = s.tx_dropped;
    portEXIT_CRITICAL(&s_lock);
}

size_t sofa_ble_seen(sofa_seen_info_t *out, size_t max)
{
    if (out == NULL) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    size_t n = (s.seen_count < max) ? s.seen_count : max;
    memcpy(out, s.seen, n * sizeof(*out));
    portEXIT_CRITICAL(&s_lock);

    /* Most-seen first: whoever opens `sofa list` is looking for the device the
     * board has been talking to, and that is the one at the top. Selection sort
     * over at most SOFA_SEEN_MAX entries. */
    for (size_t i = 0; i + 1 < n; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++) {
            if (out[j].seen_count > out[best].seen_count) {
                best = j;
            }
        }
        if (best != i) {
            sofa_seen_info_t tmp = out[i];
            out[i] = out[best];
            out[best] = tmp;
        }
    }
    return n;
}

bool sofa_ble_last_status(sofa_status_t *out)
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool have = s.have_status;
    if (have) {
        *out = s.last_status;
    }
    portEXIT_CRITICAL(&s_lock);
    return have;
}

/* --------------------------------------------------------- scanning table */

static size_t seen_index(const uint8_t addr[6], bool *is_new)
{
    char buf[SOFA_ADDR_STR_LEN];
    addr_to_str(addr, buf);

    for (size_t i = 0; i < s.seen_count; i++) {
        if (strcmp(buf, s.seen[i].addr) == 0) {
            *is_new = false;
            return i;
        }
    }
    if (s.seen_count < SOFA_SEEN_MAX) {
        *is_new = true;
        return s.seen_count++;
    }

    /*
     * Full. Evict the least-seen entry, and report it as new so the caller
     * resets the slot completely.
     *
     * This used to always overwrite slot 0 and keep its `matched` flag and
     * `seen_count`, which meant an unrelated device could be listed as a sofa
     * while the address we were actually chasing got silently evicted. In a
     * room with dozens of advertisers the ones worth keeping are the ones we
     * keep seeing, and the sofa is firmly in that group.
     */
    size_t victim = 0;
    for (size_t i = 1; i < s.seen_count; i++) {
        if (s.seen[i].seen_count < s.seen[victim].seen_count) {
            victim = i;
        }
    }
    *is_new = true;
    return victim;
}

/* ------------------------------------------------------------ advertisement */

/*
 * One advertisement (or scan response) report, inspected.
 *
 * The two flags are kept separate because the mismatch between them is the
 * interesting case: a device advertising the sofa's service with some other
 * name is what this board spent a whole evening trying to connect to.
 */
typedef struct {
    char name[SOFA_NAME_STR_LEN];
    bool name_match;   /* name starts with CONFIG_SOFA_DEVICE_NAME_PREFIX */
    bool svc_match;    /* advertises the 0xAE30 service */
} sofa_adv_t;

static void adv_inspect(const struct ble_gap_disc_desc *disc, sofa_adv_t *out)
{
    struct ble_hs_adv_fields fields;

    memset(out, 0, sizeof(*out));

    /*
     * A non-zero return means the report held more (or odder) data than the
     * fixed arrays can hold - but the parsing is best effort and the fields it
     * did fill are still valid, so use them instead of discarding the report.
     */
    int rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
    if (rc != 0 && rc != BLE_HS_EMSGSIZE) {
        ESP_LOGD(TAG, "adv parse rc=%d", rc);
    }

    const char *prefix = CONFIG_SOFA_DEVICE_NAME_PREFIX;
    size_t prefix_len = strlen(prefix);
    if (fields.name != NULL && fields.name_len > 0) {
        size_t n = fields.name_len;
        if (n >= sizeof(out->name)) {
            n = sizeof(out->name) - 1;
        }
        memcpy(out->name, fields.name, n);
        out->name[n] = '\0';

        if (prefix_len > 0 && fields.name_len >= prefix_len &&
            strncmp((const char *)fields.name, prefix, prefix_len) == 0) {
            out->name_match = true;
        }
    }

    for (uint8_t i = 0; i < fields.num_uuids16; i++) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == SOFA_SERVICE_UUID16) {
            out->svc_match = true;
            break;
        }
    }
    for (uint8_t i = 0; !out->svc_match && i < fields.num_uuids128; i++) {
        if (uuid_matches(&fields.uuids128[i].u, &s_uuid_svc.u, SOFA_SERVICE_UUID16)) {
            out->svc_match = true;
        }
    }
}

/*
 * Should we connect to this advertiser?
 *
 * With a name prefix configured the NAME decides, and the service UUID is only
 * a hint for logging. That is what the original app does - it filters the scan
 * by service UUID and then requires /Sofa_wwt/ in the name - and matching on
 * either was a bug: an unrelated device in range advertised 0xAE30 with no
 * name, so the board latched onto it and retried a connection that could never
 * succeed, forever.
 *
 * Clear the prefix to fall back to the service UUID alone.
 */
static bool adv_is_sofa(const sofa_adv_t *adv)
{
    return (strlen(CONFIG_SOFA_DEVICE_NAME_PREFIX) > 0) ? adv->name_match
                                                        : adv->svc_match;
}

static void remember(const struct ble_gap_disc_desc *disc,
                     const sofa_adv_t *adv, bool is_sofa)
{
    bool is_new = false;
    char addr[SOFA_ADDR_STR_LEN];
    addr_to_str(disc->addr.val, addr);

    portENTER_CRITICAL(&s_lock);
    size_t i = seen_index(disc->addr.val, &is_new);
    sofa_seen_info_t *slot = &s.seen[i];

    if (is_new) {
        /* A fresh entry, or an evicted slot being reused: wipe it either way,
         * so nothing of the previous occupant (its name, its matched flag, its
         * count) leaks into this one. */
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->addr, addr, sizeof(slot->addr));
    }
    if (adv->name[0] != '\0') {
        snprintf(slot->name, sizeof(slot->name), "%s", adv->name);
    }
    slot->rssi = disc->rssi;
    slot->matched = is_sofa;
    slot->seen_count++;
    s.seen_type[i] = disc->addr.type;

    /* Copy what we want to log out of the shared table before releasing the
     * lock - logging inside a critical section is not an option. */
    char log_name[SOFA_NAME_STR_LEN];
    memcpy(log_name, slot->name, sizeof(log_name));
    int8_t log_rssi = slot->rssi;
    portEXIT_CRITICAL(&s_lock);

    if (!is_new) {
        return;
    }

    if (is_sofa) {
        ESP_LOGI(TAG, "saw %s  name=\"%s\"  rssi=%d  <- sofa", addr,
                 log_name[0] ? log_name : "?", log_rssi);
    } else if (adv->svc_match) {
        /* Always logged, not gated on the noisy option: this is the one line
         * that explains why the board is not connecting to something that
         * looks exactly like a sofa. */
        ESP_LOGW(TAG, "%s advertises the sofa service 0x%04x but its name is \"%s\", "
                      "which does not start with \"%s\" -- ignoring it",
                 addr, SOFA_SERVICE_UUID16, log_name[0] ? log_name : "(none)",
                 CONFIG_SOFA_DEVICE_NAME_PREFIX);
    }
#if CONFIG_SOFA_ANNOUNCE_NEW_DEVICES
    else {
        ESP_LOGI(TAG, "saw %s  name=\"%s\"  rssi=%d", addr,
                 log_name[0] ? log_name : "?", log_rssi);
    }
#endif
}

/* ---------------------------------------------------------------- discovery */

static void fail_link(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void fail_link(const char *fmt, ...)
{
    va_list ap;
    char msg[96];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGE(TAG, "%s", msg);

    if (s.conn_handle != 0 && s.connected) {
        ble_gap_terminate(s.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void handshake_and_go_ready(void)
{
    set_state(SOFA_STATE_READY);

    ESP_LOGI(TAG, "link ready: service %u..%u, write value handle %u, notify value handle %u",
             s.svc_start, s.svc_end, s.chr_write_val, s.chr_notify_val);

#if CONFIG_SOFA_HANDSHAKE
    uint8_t frame[SOFA_FRAME_LEN];
    size_t len = sofa_frame_handshake(frame);
    /* The app writes this before it enables any control. We do not know whether
     * the sofa insists on it, so it is on by default (CONFIG_SOFA_HANDSHAKE). */
    esp_err_t err = sofa_ble_send(frame, len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "sent handshake fe 5a");
    } else {
        ESP_LOGW(TAG, "handshake not queued: %s", esp_err_to_name(err));
    }
#endif
}

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "subscribed on the notify characteristic (CCCD wrote 0x0001)");
        handshake_and_go_ready();
        return 0;
    }

    if (!s.tried_indicate) {
        /* The app's native module has an "indicate" branch that it never
         * actually reaches, so a sofa that only accepts the indication bit is
         * a real possibility worth one retry. */
        s.tried_indicate = true;
        ESP_LOGW(TAG, "CCCD write 0x0001 failed (status=%d); retrying with 0x0002",
                 error->status);
        uint8_t value[2] = {0x02, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, s.cccd_handle, value,
                                      sizeof(value), cccd_write_cb, NULL);
        if (rc != 0) {
            fail_link("CCCD retry not started; rc=%d", rc);
        }
        return 0;
    }

    fail_link("could not subscribe to notifications (status=%d)", error->status);
    return 0;
}

static void subscribe(void)
{
    if (s.cccd_handle == 0) {
        ESP_LOGW(TAG, "notify characteristic has no CCCD; proceeding anyway");
        handshake_and_go_ready();
        return;
    }

    uint8_t value[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(s.conn_handle, s.cccd_handle, value,
                                  sizeof(value), cccd_write_cb, NULL);
    if (rc != 0) {
        fail_link("subscribe write not started; rc=%d", rc);
    }
}

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            BLE_UUID16(&dsc->uuid.u)->value == SOFA_CCCD_UUID16) {
            s.cccd_handle = dsc->handle;
            ESP_LOGI(TAG, "CCCD handle %u", s.cccd_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        subscribe();
        return 0;
    }

    fail_link("descriptor discovery failed; status=%d", error->status);
    return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        if (uuid_matches(&chr->uuid.u, &s_uuid_write.u, SOFA_WRITE_UUID16)) {
            s.chr_write_val = chr->val_handle;
            s.have_write_chr = true;
        } else if (uuid_matches(&chr->uuid.u, &s_uuid_notify.u, SOFA_NOTIFY_UUID16)) {
            s.chr_notify_val = chr->val_handle;
            s.have_notify_chr = true;
            ESP_LOGI(TAG, "notify characteristic properties=0x%02x (%s%s%s)",
                     chr->properties,
                     (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) ? "notify" : "",
                     (chr->properties & BLE_GATT_CHR_PROP_INDICATE) ? " indicate" : "",
                     (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) ? " write-no-rsp" : "");
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!s.have_write_chr) {
            fail_link("write characteristic 0x%04x not found", SOFA_WRITE_UUID16);
            return 0;
        }
        if (!s.have_notify_chr) {
            fail_link("notify characteristic 0x%04x not found", SOFA_NOTIFY_UUID16);
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn_handle, s.chr_notify_val, s.svc_end,
                                         dsc_disc_cb, NULL);
        if (rc != 0) {
            fail_link("descriptor discovery not started; rc=%d", rc);
        }
        return 0;
    }

    fail_link("characteristic discovery failed; status=%d", error->status);
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;

    if (error->status == 0 && svc != NULL) {
        if (uuid_matches(&svc->uuid.u, &s_uuid_svc.u, SOFA_SERVICE_UUID16)) {
            s.svc_start = svc->start_handle;
            s.svc_end = svc->end_handle;
            s.have_svc = true;
            ESP_LOGI(TAG, "service 0x%04x: handles %u..%u", SOFA_SERVICE_UUID16,
                     s.svc_start, s.svc_end);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (!s.have_svc) {
            fail_link("service 0x%04x not found", SOFA_SERVICE_UUID16);
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn_handle, s.svc_start, s.svc_end,
                                         chr_disc_cb, NULL);
        if (rc != 0) {
            fail_link("characteristic discovery not started; rc=%d", rc);
        }
        return 0;
    }

    fail_link("service discovery failed; status=%d", error->status);
    return 0;
}

/* ------------------------------------------------------------------ connect */

static void begin_discovery(void)
{
    set_state(SOFA_STATE_DISCOVERING);
    s.have_svc = false;
    s.have_write_chr = false;
    s.have_notify_chr = false;
    s.tried_indicate = false;
    s.cccd_handle = 0;

    int rc = ble_gattc_disc_all_svcs(s.conn_handle, svc_disc_cb, NULL);
    if (rc != 0) {
        fail_link("service discovery not started; rc=%d", rc);
    }
}

static void connect_to(const ble_addr_t *addr, int8_t rssi)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    addr_to_str(addr->val, s.peer_addr);
    s.peer_rssi = rssi;
    s.peer_addr_type = addr->type;
    portEXIT_CRITICAL(&s_lock);

    /*
     * Leave SCANNING before the connect call, not after. Cancelling our own
     * scan makes the controller post BLE_GAP_EVENT_DISC_COMPLETE, and that
     * handler treats "scan ended while still scanning" as a fault worth
     * restarting - which would fight the connection we are about to open.
     */
    set_state(SOFA_STATE_CONNECTING);

    rc = ble_gap_connect(own_addr_type, addr, CONFIG_SOFA_CONNECT_TIMEOUT_MS, NULL,
                         gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        schedule_retry();
        return;
    }
    ESP_LOGI(TAG, "connecting to %s (rssi %d)", s.peer_addr, rssi);
}

esp_err_t sofa_ble_connect_to(const char *addr_str)
{
    uint8_t val[6];
    if (!addr_from_str(addr_str, val)) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalised[SOFA_ADDR_STR_LEN];
    addr_to_str(val, normalised);

    /* The caller is not the NimBLE host task, so do not touch GAP here: hand
     * the request over through s_ctl_ev and let the host task run it. */
    portENTER_CRITICAL(&s_lock);
    if (s.connected) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctl_addr.type = BLE_ADDR_PUBLIC;
    for (size_t i = 0; i < s.seen_count; i++) {
        if (strcmp(s.seen[i].addr, normalised) == 0) {
            s_ctl_addr.type = s.seen_type[i];
            break;
        }
    }
    memcpy(s_ctl_addr.val, val, sizeof(val));
    s_ctl_kind = CTL_CONNECT;
    portEXIT_CRITICAL(&s_lock);

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ctl_ev);
    return ESP_OK;
}

/* -------------------------------------------------------------------- scan */

static void start_scan(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    struct ble_gap_disc_params disc_params = {0};
    /*
     * The sofa puts the 0xAE30 service in the advertisement and its name in the
     * scan response - the same split the Android app relies on when it turns
     * startLeScan(serviceUuids, ...) into an active scan. So: active scan, and
     * do NOT let the controller filter duplicates, or the scan response that
     * carries the name never reaches us.
     */
    disc_params.passive = 0;
    disc_params.filter_duplicates = 0;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    /*
     * Scan duty cycle. Leaving these at 0 asks for the controller default,
     * which is a 10 ms window every 10 ms - a scan that never stops.
     *
     * On a single-radio chip that is airtime taken straight from WiFi, and
     * there is nothing to buy with it: measured here, 30 ms out of every 100 ms
     * still found every advertiser in the room within about three seconds,
     * including ones 90 dB down in the noise.
     *
     * (An earlier version of this comment blamed a five-second HTTP delay on
     * this setting. That was wrong - the delay was macOS's mDNS resolver, and
     * by IP the same request takes ~0.2 s either way. The duty cycle is about
     * airtime, not latency.)
     */
    uint16_t interval = (uint16_t)((CONFIG_SOFA_SCAN_INTERVAL_MS * 1000ull) / 625ull);
    uint16_t window = (uint16_t)((CONFIG_SOFA_SCAN_WINDOW_MS * 1000ull) / 625ull);
    if (window > interval) {
        ESP_LOGW(TAG, "scan window %d ms > interval %d ms; clamping",
                 CONFIG_SOFA_SCAN_WINDOW_MS, CONFIG_SOFA_SCAN_INTERVAL_MS);
        window = interval;
    }
    disc_params.itvl = interval;
    disc_params.window = window;

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        set_state(SOFA_STATE_SCANNING);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "scanning for \"%s*\" or service 0x%04x every %d ms (%d ms window)%s",
             CONFIG_SOFA_DEVICE_NAME_PREFIX, SOFA_SERVICE_UUID16,
             CONFIG_SOFA_SCAN_INTERVAL_MS, CONFIG_SOFA_SCAN_WINDOW_MS,
             (strcmp(CONFIG_SOFA_TARGET_ADDR, "ADDR_ANY") != 0) ? " (address pinned)" : "");
    set_state(SOFA_STATE_SCANNING);
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    start_scan();
}

static void schedule_retry(void)
{
    portENTER_CRITICAL(&s_lock);
    s.connected = false;
    s.conn_handle = 0;
    portEXIT_CRITICAL(&s_lock);

    set_state(SOFA_STATE_RETRY_WAIT);
    (void)esp_timer_stop(s_retry_timer);
    uint64_t us = (uint64_t)CONFIG_SOFA_RETRY_DELAY_MS * 1000ull;
    if (us == 0) {
        us = 1000;
    }
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, us));
}

void sofa_ble_scan_restart(void)
{
    portENTER_CRITICAL(&s_lock);
    s_ctl_kind = CTL_RESTART_SCAN;
    portEXIT_CRITICAL(&s_lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ctl_ev);
}

/* Runs on the NimBLE host task, which is the only place GAP procedures may be
 * started from. */
static void ctl_ev_cb(struct ble_npl_event *ev)
{
    (void)ev;

    sofa_ctl_kind_t kind;
    ble_addr_t addr;

    portENTER_CRITICAL(&s_lock);
    kind = s_ctl_kind;
    s_ctl_kind = CTL_NONE;
    addr = s_ctl_addr;
    portEXIT_CRITICAL(&s_lock);

    switch (kind) {
    case CTL_CONNECT:
        (void)ble_gap_disc_cancel();
        connect_to(&addr, 0);
        break;

    case CTL_RESTART_SCAN:
        (void)esp_timer_stop(s_retry_timer);
        if (s.connected && s.conn_handle != 0) {
            /* Let the DISCONNECT event schedule the rescan - that keeps the
             * "everything resets in one place" rule intact. */
            ble_gap_terminate(s.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            (void)ble_gap_disc_cancel();
            start_scan();
        }
        break;

    default:
        break;
    }
}

static void handle_disc(const struct ble_gap_disc_desc *disc)
{
    sofa_adv_t adv;
    adv_inspect(disc, &adv);

    bool is_sofa = adv_is_sofa(&adv);
    remember(disc, &adv, is_sofa);

    if (sofa_ble_state() != SOFA_STATE_SCANNING) {
        return;   /* already connecting; keep recording for `sofa list` */
    }

    const char *why;
    if (strcmp(CONFIG_SOFA_TARGET_ADDR, "ADDR_ANY") != 0) {
        uint8_t want[6];
        if (!addr_from_str(CONFIG_SOFA_TARGET_ADDR, want) ||
            memcmp(want, disc->addr.val, sizeof(want)) != 0) {
            return;
        }
        why = "pinned address";
    } else if (is_sofa) {
        why = adv.name_match ? "name" : "service 0xae30";
    } else {
        return;
    }

    ESP_LOGI(TAG, "found %s (matched on %s, rssi %d)",
             adv.name[0] ? adv.name : "unnamed device", why, disc->rssi);

    (void)ble_gap_disc_cancel();
    connect_to(&disc->addr, disc->rssi);
}

/* --------------------------------------------------------------------- tx */

static void tx_retry_timer_cb(void *arg)
{
    (void)arg;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
}

static void tx_ev_cb(struct ble_npl_event *ev)
{
    (void)ev;

    sofa_frame_t frame;
    while (xQueueReceive(s_txq, &frame, 0) == pdTRUE) {
        if (sofa_ble_state() != SOFA_STATE_READY) {
            portENTER_CRITICAL(&s_lock);
            s.tx_dropped++;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        int rc = ble_gattc_write_no_rsp_flat(s.conn_handle, s.chr_write_val,
                                             frame.data, frame.len);
        if (rc == BLE_HS_ENOMEM) {
            /* Controller buffers are momentarily exhausted. Put the frame back
             * and come back in a moment - a button press is worth keeping. */
            xQueueSendToFront(s_txq, &frame, 0);
            (void)esp_timer_stop(s_tx_retry_timer);
            (void)esp_timer_start_once(s_tx_retry_timer, SOFA_TX_RETRY_US);
            return;
        }

        portENTER_CRITICAL(&s_lock);
        if (rc != 0) {
            s.tx_dropped++;
        } else {
            s.tx_sent++;
        }
        portEXIT_CRITICAL(&s_lock);

        if (rc != 0) {
            ESP_LOGW(TAG, "write failed; rc=%d", rc);
        } else {
            char hex[3 * SOFA_TX_FRAME_MAX];
            sofa_hex(frame.data, frame.len, hex, sizeof(hex));
            ESP_LOGI(TAG, "tx %s", hex);
        }
    }
}

esp_err_t sofa_ble_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > SOFA_TX_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sofa_ble_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    sofa_frame_t frame = {.len = (uint8_t)len};
    memcpy(frame.data, data, len);

    if (xQueueSend(s_txq, &frame, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        s.tx_dropped++;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    return ESP_OK;
}

void sofa_ble_flush_tx(void)
{
    sofa_frame_t frame;
    while (xQueueReceive(s_txq, &frame, 0) == pdTRUE) {
        /* drained */
    }
}

/* ------------------------------------------------------------------ notify */

static void handle_notify(const struct ble_gap_event *event)
{
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    uint8_t buf[64];

    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) != 0) {
        return;
    }

    char hex[3 * sizeof(buf)];
    sofa_hex(buf, len, hex, sizeof(hex));

    portENTER_CRITICAL(&s_lock);
    s.notify_count++;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "rx %s%s", hex, event->notify_rx.indication ? " (indication)" : "");

    sofa_status_t status;
    if (!sofa_status_parse(buf, len, &status)) {
        return;
    }
    if (status.has_mode) {
        ESP_LOGI(TAG, "  movement mode: %s", sofa_mode_name(status.mode));
    }
    if (status.has_dir) {
        ESP_LOGI(TAG, "  movement direction: %s", sofa_dir_name(status.dir));
    }
    if (status.has_motor_stop) {
        ESP_LOGI(TAG, "  motor stopped: %s", sofa_motor_name(status.motor));
        portENTER_CRITICAL(&s_lock);
        s.motor_stop_count++;
        s.last_motor = status.motor;
        portEXIT_CRITICAL(&s_lock);
    }

    /*
     * Merge rather than replace: the sofa sends mode+direction in one frame and
     * motor-stop in another, so overwriting would lose the mode the moment a
     * run finished.
     */
    portENTER_CRITICAL(&s_lock);
    if (status.has_mode) {
        s.last_status.has_mode = true;
        s.last_status.mode = status.mode;
    }
    if (status.has_dir) {
        s.last_status.has_dir = true;
        s.last_status.dir = status.dir;
    }
    if (status.has_motor_stop) {
        s.last_status.has_motor_stop = true;
        s.last_status.motor = status.motor;
    }
    s.have_status = true;
    portEXIT_CRITICAL(&s_lock);

    sofa_status_cb_t cb;
    portENTER_CRITICAL(&s_lock);
    cb = s.status_cb;
    portEXIT_CRITICAL(&s_lock);
    if (cb != NULL) {
        cb(&status);
    }
}

/* ------------------------------------------------------------- GAP plumbing */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_disc(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGD(TAG, "scan stopped; reason=%d", event->disc_complete.reason);
        if (sofa_ble_state() == SOFA_STATE_SCANNING) {
            schedule_retry();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            portENTER_CRITICAL(&s_lock);
            s.conn_handle = event->connect.conn_handle;
            s.connected = true;
            char peer[SOFA_ADDR_STR_LEN];
            memcpy(peer, s.peer_addr, sizeof(peer));
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "connected to %s", peer);
            begin_discovery();
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            schedule_retry();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        char peer[SOFA_ADDR_STR_LEN];
        portENTER_CRITICAL(&s_lock);
        memcpy(peer, s.peer_addr, sizeof(peer));
        portEXIT_CRITICAL(&s_lock);
        /* Name the peer: when the board keeps looping on one address that will
         * not connect, that address is the whole answer. */
        ESP_LOGW(TAG, "disconnected from %s; reason=%d", peer,
                 event->disconnect.reason);
        schedule_retry();
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        handle_notify(event);
        return 0;

    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&s_lock);
        s.mtu = event->mtu.value;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "MTU %u (channel %d)", event->mtu.value, event->mtu.channel_id);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGD(TAG, "connection parameters updated; status=%d",
                 event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Nothing here bonds, but a peripheral with a stale bond would
         * otherwise refuse to talk; drop ours and accept the new link. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        return;
    }
    start_scan();
}

/* -------------------------------------------------------------------- init */

esp_err_t sofa_ble_start(void)
{
    int rc = ble_uuid_from_str(&s_uuid_svc, SOFA_SERVICE_UUID_STR);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_uuid_from_str(&s_uuid_write, SOFA_WRITE_UUID_STR);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_uuid_from_str(&s_uuid_notify, SOFA_NOTIFY_UUID_STR);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s.state = SOFA_STATE_BOOT;
    s_txq = xQueueCreate(SOFA_TX_QUEUE_LEN, sizeof(sofa_frame_t));
    if (s_txq == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t retry_args = {
        .callback = &retry_timer_cb,
        .name = "sofa-rescan",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));

    const esp_timer_create_args_t tx_retry_args = {
        .callback = &tx_retry_timer_cb,
        .name = "sofa-tx-retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tx_retry_args, &s_tx_retry_timer));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Handed to the host task's own event queue by sofa_ble_send(). */
    ble_npl_event_init(&s_tx_ev, tx_ev_cb, NULL);
    /* Same, for requests that may only run on the host task. */
    ble_npl_event_init(&s_ctl_ev, ctl_ev_cb, NULL);

    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
