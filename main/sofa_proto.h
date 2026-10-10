/*
 * sofa_proto - the sofa's BLE wire format, kept free of any BLE or ESP-IDF
 * dependency so it can be read, tested and changed on its own.
 *
 * Everything here is observed from the shipped Android app
 * (assets/widget/html) and its bundled native module
 * (com.apicloud.uzble). Frames are two bytes unless noted, written to the
 * AE01 characteristic as Write Without Response.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Button codes.
 *
 * The low nibble is "up" for seats 1..4, the high nibble is "down". The app's
 * on-screen buttons carry these exact ids, and the two "all" codes follow the
 * same pattern (0x55 = 0101 0101, 0x66 = 0110 0110).
 */
typedef enum {
    SOFA_BTN_SEAT1_UP   = 0x01,
    SOFA_BTN_SEAT1_DOWN = 0x10,
    SOFA_BTN_SEAT2_UP   = 0x02,
    SOFA_BTN_SEAT2_DOWN = 0x20,
    SOFA_BTN_SEAT3_UP   = 0x04,
    SOFA_BTN_SEAT3_DOWN = 0x40,
    SOFA_BTN_SEAT4_UP   = 0x08,
    SOFA_BTN_SEAT4_DOWN = 0x80,
    SOFA_BTN_ALL_UP     = 0x55,
    SOFA_BTN_ALL_DOWN   = 0x66,
} sofa_button_t;

/* Number of bytes a control frame occupies on the wire. */
#define SOFA_FRAME_LEN 2

/* "1".."4" -> seat up, "1d".."4d" -> seat down, "all"/"55" and "66" -> all. */
bool sofa_button_from_name(const char *name, sofa_button_t *out);

/*
 * The console and the HTTP API both address a target the same way: "all" for
 * the whole sofa, "1".."4" for one seat. This fills in that target's raise and
 * lower codes.
 */
bool sofa_target_buttons(const char *name, sofa_button_t *up, sofa_button_t *down);

/* True for both of the "all seats" codes. */
bool sofa_button_is_all(sofa_button_t btn);

const char *sofa_button_name(sofa_button_t btn);

/*
 * Frame builders. Each writes SOFA_FRAME_LEN bytes and returns that length.
 *
 *   fe 5a          handshake, written right after subscribing
 *   fa <btn>       press, one-shot mode (sofa runs to its end stop / toggles)
 *   f8 <btn>       press, jog mode (sofa moves only while this repeats)
 *   fb <btn>       release, jog mode
 *   f1 00 | f1 01  movement mode: one-shot | jog
 *   f2 00 | f2 01  movement direction: forward | reverse
 */
size_t sofa_frame_handshake(uint8_t out[SOFA_FRAME_LEN]);
size_t sofa_frame_press(sofa_button_t btn, bool jog, uint8_t out[SOFA_FRAME_LEN]);
size_t sofa_frame_release(sofa_button_t btn, uint8_t out[SOFA_FRAME_LEN]);
size_t sofa_frame_mode(bool one_shot, uint8_t out[SOFA_FRAME_LEN]);
size_t sofa_frame_direction(bool forward, uint8_t out[SOFA_FRAME_LEN]);

/*
 * Status frames pushed by the sofa on the AE02 characteristic. The app reads
 * them positionally and so do we - see sofa_status_parse.
 */
typedef struct {
    bool has_mode;
    uint8_t mode;   /* 1 = one-shot, anything else = jog */

    bool has_dir;
    uint8_t dir;    /* 1 = forward, anything else = reverse */

    bool has_motor_stop;
    uint8_t motor;  /* 1..4 = that seat, 5 = "all seats together" */
} sofa_status_t;

/*
 * Parses one notification.
 *
 * Mirrors the app exactly rather than being clever about it:
 *   bytes[0] == f5 -> mode  = bytes[1]
 *   bytes[2] == f6 -> dir   = bytes[3]
 *   bytes[0] == f7 -> motor = bytes[1]
 * A 4-byte f5 xx f6 yy frame therefore reports both mode and direction, and
 * the caller is expected to check the individual has_* flags.
 *
 * Returns true if at least one field was recognised.
 */
bool sofa_status_parse(const uint8_t *data, size_t len, sofa_status_t *out);

const char *sofa_mode_name(uint8_t mode);
const char *sofa_dir_name(uint8_t dir);
const char *sofa_motor_name(uint8_t motor);

/* "f5 01 f6 01" style rendering. Returns the number of bytes written. */
size_t sofa_hex(const uint8_t *data, size_t len, char *dst, size_t dst_size);
