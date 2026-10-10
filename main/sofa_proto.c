#include "sofa_proto.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/*
 * Frame opcodes. Kept as #defines rather than an enum so the pairing between
 * an opcode and the function that emits it stays obvious at the use site.
 */
#define OP_HANDSHAKE 0xfe
#define OP_PRESS_ONESHOT 0xfa
#define OP_PRESS_JOG 0xf8
#define OP_RELEASE_JOG 0xfb
#define OP_MODE 0xf1
#define OP_DIRECTION 0xf2

/* Status frame markers pushed by the sofa. */
#define ST_MODE 0xf5
#define ST_DIR 0xf6
#define ST_MOTOR_STOP 0xf7

/* Index of the opcode byte in a "f1 00" style configuration frame. */
#define CFG_VALUE_0 0x00
#define CFG_VALUE_1 0x01

bool sofa_button_from_name(const char *name, sofa_button_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }

    if (strcasecmp(name, "all") == 0 || strcasecmp(name, "allup") == 0 ||
        strcasecmp(name, "55") == 0) {
        *out = SOFA_BTN_ALL_UP;
        return true;
    }
    if (strcasecmp(name, "alldown") == 0 || strcasecmp(name, "66") == 0) {
        *out = SOFA_BTN_ALL_DOWN;
        return true;
    }

    /* "1" .. "4" for an upward seat, "1d" .. "4d" for the same seat downward. */
    if (name[0] < '1' || name[0] > '4') {
        return false;
    }
    int seat = name[0] - '0';

    if (name[1] == '\0' || strcasecmp(name + 1, "u") == 0 ||
        strcasecmp(name + 1, "up") == 0) {
        *out = (sofa_button_t)(1u << (seat - 1));  /* 0x01, 0x02, 0x04, 0x08 */
        return true;
    }
    if (strcasecmp(name + 1, "d") == 0 || strcasecmp(name + 1, "down") == 0) {
        *out = (sofa_button_t)(0x10u << (seat - 1)); /* 0x10, 0x20, 0x40, 0x80 */
        return true;
    }
    return false;
}

bool sofa_button_is_all(sofa_button_t btn)
{
    return btn == SOFA_BTN_ALL_UP || btn == SOFA_BTN_ALL_DOWN;
}

bool sofa_target_buttons(const char *name, sofa_button_t *up, sofa_button_t *down)
{
    if (name == NULL || up == NULL || down == NULL) {
        return false;
    }
    if (strcasecmp(name, "all") == 0) {
        *up = SOFA_BTN_ALL_UP;
        *down = SOFA_BTN_ALL_DOWN;
        return true;
    }
    if (name[0] >= '1' && name[0] <= '4' && name[1] == '\0') {
        int seat = name[0] - '0';
        *up = (sofa_button_t)(1u << (seat - 1));
        *down = (sofa_button_t)(0x10u << (seat - 1));
        return true;
    }
    return false;
}

const char *sofa_button_name(sofa_button_t btn)
{
    switch (btn) {
    case SOFA_BTN_SEAT1_UP:   return "seat1-up";
    case SOFA_BTN_SEAT1_DOWN: return "seat1-down";
    case SOFA_BTN_SEAT2_UP:   return "seat2-up";
    case SOFA_BTN_SEAT2_DOWN: return "seat2-down";
    case SOFA_BTN_SEAT3_UP:   return "seat3-up";
    case SOFA_BTN_SEAT3_DOWN: return "seat3-down";
    case SOFA_BTN_SEAT4_UP:   return "seat4-up";
    case SOFA_BTN_SEAT4_DOWN: return "seat4-down";
    case SOFA_BTN_ALL_UP:     return "all-up";
    case SOFA_BTN_ALL_DOWN:   return "all-down";
    default:                  return "unknown";
    }
}

size_t sofa_frame_handshake(uint8_t out[SOFA_FRAME_LEN])
{
    out[0] = OP_HANDSHAKE;
    out[1] = 0x5a;
    return SOFA_FRAME_LEN;
}

size_t sofa_frame_press(sofa_button_t btn, bool jog, uint8_t out[SOFA_FRAME_LEN])
{
    out[0] = jog ? OP_PRESS_JOG : OP_PRESS_ONESHOT;
    out[1] = (uint8_t)btn;
    return SOFA_FRAME_LEN;
}

size_t sofa_frame_release(sofa_button_t btn, uint8_t out[SOFA_FRAME_LEN])
{
    out[0] = OP_RELEASE_JOG;
    out[1] = (uint8_t)btn;
    return SOFA_FRAME_LEN;
}

size_t sofa_frame_mode(bool one_shot, uint8_t out[SOFA_FRAME_LEN])
{
    out[0] = OP_MODE;
    out[1] = one_shot ? CFG_VALUE_0 : CFG_VALUE_1;
    return SOFA_FRAME_LEN;
}

size_t sofa_frame_direction(bool forward, uint8_t out[SOFA_FRAME_LEN])
{
    out[0] = OP_DIRECTION;
    out[1] = forward ? CFG_VALUE_0 : CFG_VALUE_1;
    return SOFA_FRAME_LEN;
}

bool sofa_status_parse(const uint8_t *data, size_t len, sofa_status_t *out)
{
    if (data == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    /*
     * The app tests bytes[0] and bytes[2] independently, and only inside a
     * "longer than 3 bytes" guard for the mode field. Reproduce both checks
     * unconditionally so a shorter or oddly framed notification still yields
     * whatever it unambiguously encodes.
     */
    if (len >= 2 && data[0] == ST_MODE) {
        out->has_mode = true;
        out->mode = data[1];
    }
    if (len >= 4 && data[2] == ST_DIR) {
        out->has_dir = true;
        out->dir = data[3];
    }
    if (len >= 2 && data[0] == ST_MOTOR_STOP) {
        out->has_motor_stop = true;
        out->motor = data[1];
    }

    return out->has_mode || out->has_dir || out->has_motor_stop;
}

const char *sofa_mode_name(uint8_t mode)
{
    return (mode == 1) ? "one-shot (一键启停)" : "jog (点动)";
}

const char *sofa_dir_name(uint8_t dir)
{
    return (dir == 1) ? "forward (正向)" : "reverse (反向)";
}

const char *sofa_motor_name(uint8_t motor)
{
    switch (motor) {
    case 1:  return "seat 1";
    case 2:  return "seat 2";
    case 3:  return "seat 3";
    case 4:  return "seat 4";
    case 5:  return "all seats together";
    default: return "unknown motor";
    }
}

size_t sofa_hex(const uint8_t *data, size_t len, char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < len; i++) {
        int n = snprintf(dst + written, dst_size - written, i ? " %02x" : "%02x",
                         data[i]);
        if (n < 0 || (size_t)n >= dst_size - written) {
            break;
        }
        written += (size_t)n;
    }
    return written;
}
