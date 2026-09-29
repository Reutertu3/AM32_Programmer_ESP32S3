/*
 * led.h — status LED.
 *
 *   off               waiting for MSP
 *   solid on          4-way passthrough session active
 *   flashing          ESC flash/EEPROM being read, written or erased
 *
 * All calls are no-ops when ESC4W_LED_PIN is -1.
 */
#pragma once

#include <stdbool.h>

void led_init(void);

/* Set a steady state. Cancels any activity flashing in progress. */
void led_set(bool on);

/* Mark one ESC data transaction. Each call toggles the LED; once no
 * call has arrived for ESC4W_LED_ACTIVITY_HOLD_MS it settles back to
 * solid on. */
void led_activity(void);
