/*
 * sofa_ble - the NimBLE GATT *client* side of talking to the sofa.
 *
 * The reverse engineered app is the central; so is this. It scans for a
 * peripheral advertising the 0xAE30 service, connects, discovers the 0xAE01
 * (write) and 0xAE02 (notify) characteristics, enables notifications and then
 * writes 2-byte command frames with Write Without Response - the write type the
 * app's native module uses for every command it sends.
 *
 * Everything the caller has to know is in this header; the discovery state
 * machine and the TX marshalling stay private to the .c file.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "sofa_proto.h"

/* Largest frame the link accepts. The real commands are 2 bytes; the extra room
 * is for experimenting with the app's commented-out serial frame. */
#define SOFA_TX_FRAME_MAX 20

#define SOFA_ADDR_STR_LEN 18
#define SOFA_NAME_STR_LEN 32
#define SOFA_SEEN_MAX 16

typedef enum {
    SOFA_STATE_BOOT = 0,   /* stack starting, nothing done yet */
    SOFA_STATE_SCANNING,
    SOFA_STATE_CONNECTING,
    SOFA_STATE_DISCOVERING, /* connected, walking services/characteristics */
    SOFA_STATE_READY,       /* subscribed; commands will be accepted */
    SOFA_STATE_RETRY_WAIT,  /* disconnect or failure, waiting to rescan */
} sofa_state_t;

const char *sofa_state_name(sofa_state_t state);

/*
 * Called whenever the state changes. It runs on the NimBLE host task, so keep
 * it short and do not call back into sofa_ble_* from it.
 */
typedef void (*sofa_state_cb_t)(sofa_state_t state);
void sofa_ble_set_state_cb(sofa_state_cb_t cb);

/*
 * Called for every decodable status frame the sofa pushes on 0xAE02. Also runs
 * on the NimBLE host task. The raw frame has already been logged by then.
 */
typedef void (*sofa_status_cb_t)(const sofa_status_t *status);
void sofa_ble_set_status_cb(sofa_status_cb_t cb);

/* Brings up the NimBLE host and starts scanning. Returns once the host task is
 * running, not once a sofa has been found. */
esp_err_t sofa_ble_start(void);

sofa_state_t sofa_ble_state(void);
bool sofa_ble_ready(void);

/*
 * Queues one frame for Write Without Response on 0xAE01.
 *
 * Safe to call from any task: the frame is handed to the NimBLE host task,
 * because the GATT client API is not reentrant from an arbitrary task. Returns
 * ESP_ERR_INVALID_STATE when there is no ready link, ESP_ERR_TIMEOUT when the
 * queue is full.
 */
esp_err_t sofa_ble_send(const uint8_t *data, size_t len);

/* Drops everything still queued. Used when a held button is released. */
void sofa_ble_flush_tx(void);

/* Cancels whatever is in flight and starts scanning again. */
void sofa_ble_scan_restart(void);

/*
 * Connects to a specific address instead of waiting for a scan match.
 * Accepts "aa:bb:cc:dd:ee:ff"; the address type is taken from the scan table
 * when known, public otherwise.
 */
esp_err_t sofa_ble_connect_to(const char *addr_str);

typedef struct {
    char addr[SOFA_ADDR_STR_LEN];
    char name[SOFA_NAME_STR_LEN];
    int8_t rssi;
    bool matched;       /* looked like a sofa */
    uint32_t seen_count;
} sofa_seen_info_t;

/* Copies up to `max` remembered peripherals, most-seen first; returns how many
 * were written. */
size_t sofa_ble_seen(sofa_seen_info_t *out, size_t max);

/*
 * Copies the most recent decodable status frame. Returns false if the sofa has
 * not pushed one yet - which is normal for the first second after connecting.
 */
bool sofa_ble_last_status(sofa_status_t *out);

typedef struct {
    sofa_state_t state;
    bool connected;
    char peer_addr[SOFA_ADDR_STR_LEN];
    int rssi;
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t chr_write_val;
    uint16_t chr_notify_val;
    uint16_t cccd_handle;
    uint16_t mtu;
    uint32_t notify_count;
    /* Bumped every time the sofa reports a motor stopping. The dashboard uses
     * the change to clear a seat indicator it lit when it sent the command -
     * the sofa only tells us about stops, never about starts. */
    uint32_t motor_stop_count;
    uint8_t last_motor;
    uint32_t tx_sent;
    uint32_t tx_dropped;
} sofa_ble_info_t;

void sofa_ble_get_info(sofa_ble_info_t *out);
