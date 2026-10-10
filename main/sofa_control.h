/*
 * sofa_control - the intent layer between the console and the raw frames.
 *
 * It owns the two pieces of state the sofa and the app both keep:
 *   - the movement mode (one-shot "一键启停" vs jog "点动"), which the sofa
 *     pushes in its f5 status frame and which commands have to respect, and
 *   - the currently held button, because jog mode only moves while the frame
 *     keeps arriving. The app resends every 50 ms while the button is held;
 *     this does the same from an esp_timer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "sofa_ble.h"
#include "sofa_proto.h"

void sofa_control_init(void);

/*
 * Link state is fanned out by main.c, because sofa_ble only holds one state
 * callback and the status LED wants it too.
 */
void sofa_control_on_link_state(sofa_state_t state);

bool sofa_control_one_shot(void);

/* The direction the last f2 frame set, or the last one the sofa reported. */
bool sofa_control_forward(void);

/* Switches the movement mode and tells the sofa (f1 00 / f1 01). */
esp_err_t sofa_control_set_one_shot(bool one_shot);

/* Sends f2 00 / f2 01. */
esp_err_t sofa_control_set_forward(bool forward);

/* Stops a whole target, given that target's up and down button codes.
 *
 * Deliberately conservative. In one-shot mode the sofa is stopped by pressing
 * the *same* button again, so stopping a target whose direction we do not know
 * would mean pressing one of the two - and pressing the other one starts the
 * sofa moving. This therefore only acts when it can be safe:
 *   - the target is the button currently held in jog mode -> release it, or
 *   - jog mode with nothing held -> send both release frames, which are no-ops
 *     for a motor that is not running.
 * Anything else returns ESP_ERR_NOT_SUPPORTED rather than guessing.
 */
esp_err_t sofa_control_stop_target(sofa_button_t up, sofa_button_t down);

/* True while a jog press is repeating. */
bool sofa_control_is_held(void);
sofa_button_t sofa_control_held_button(void);

/* Presses `btn`, respecting the current movement mode. In jog mode this also
 * starts the repeat; the sofa keeps moving until sofa_control_stop(). */
esp_err_t sofa_control_move(sofa_button_t btn);

/* Releases whatever is held; without a held button it toggles the last button
 * off the way the app's second press does. Returns ESP_ERR_NOT_FOUND when
 * nothing has been pressed yet, so the caller can say so instead of blaming the
 * link. */
esp_err_t sofa_control_stop(void);

/* Same, for one specific button: acts on exactly that button's code. */
esp_err_t sofa_control_stop_button(sofa_button_t btn);

/* Drops any queued repeats without sending anything. Used on disconnect. */
void sofa_control_abort(void);

/*
 * Hold watchdog.
 *
 * In jog mode the firmware is the thing repeating the press frame, so if
 * whoever asked for the movement goes away mid-press - a phone that loses WiFi,
 * a browser tab that gets closed, a page that is backgrounded - the seat would
 * keep moving. Arming the watchdog makes the jog stop on its own unless it is
 * kept alive.
 *
 * 0 disables it. The console never arms it, because a person at a terminal is
 * their own watchdog; the HTTP layer arms it on every move and refreshes it on
 * every keep-alive.
 */
void sofa_control_hold_watchdog(uint32_t timeout_ms);
void sofa_control_keep_alive(void);

/* Escape hatch: queue arbitrary bytes. */
esp_err_t sofa_control_send_raw(const uint8_t *data, size_t len);
