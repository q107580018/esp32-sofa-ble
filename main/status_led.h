/*
 * status_led - the only status output that still works when the board is
 * running off a charger with no host attached, which is how it gets used.
 *
 * One input, "is this thing usable yet". app_main folds the BLE link and the
 * WiFi link into it, because from across the room that is the only question
 * worth answering in one bit.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t status_led_init(void);

/**
 * Call with true when everything the board is configured to do is working:
 * the sofa link accepts commands, and the WiFi link (if configured) is up.
 *
 * Safe to call from any task, and cheap to call repeatedly - the state is
 * latched, so an unchanged value does nothing.
 */
void status_led_set_ready(bool ready);
