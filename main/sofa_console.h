/*
 * sofa_console - a UART REPL for driving the sofa by hand.
 *
 * It exists because the board has no buttons: without it there is no way to
 * send a single command once the firmware is flashed, and no way to find the
 * sofa's address for CONFIG_SOFA_TARGET_ADDR.
 */
#pragma once

#include "esp_err.h"

/* Registers the `sofa` command and starts the REPL. */
esp_err_t sofa_console_start(void);
