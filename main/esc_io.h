/*
 * esc_io.h — half-duplex one-wire link to a single selected ESC.
 *
 * Replaces Betaflight's bit-banged suart (serial_4way_impl.h) with a
 * hardware UART whose TX/RX signals are re-routed through the GPIO
 * matrix to whichever ESC pin is currently selected. One UART, four
 * pads, one active at a time — which is exactly the "index" concept
 * the 4-way protocol needs and a plain one-wire adapter lacks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Configure all ESC pins as idle inputs and bring up the UART.
 * Returns the number of usable ESC outputs. */
uint8_t esc_io_init(void);

/* Release the UART routing and park every pin as a pulled-up input. */
void esc_io_release(void);

/* Select the active ESC (0 .. ESC4W_ESC_COUNT-1). Any previously
 * selected pin is detached from the UART and parked. */
bool esc_io_select(uint8_t index);

/* Send len bytes on the selected pin, blocking until the last stop bit
 * has left the shift register. The local echo is consumed and
 * discarded, so the RX path is clean on return. */
void esc_io_write(const uint8_t *buf, uint16_t len);

/* Read up to len bytes. timeout_ms is a per-byte timeout, matching the
 * start-bit timeout semantics of the upstream bit-bang receiver.
 * Returns the number of bytes actually read. */
uint16_t esc_io_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms);

/* Discard anything sitting in the receive path. */
void esc_io_flush(void);

/* Drive the selected pin low for ms milliseconds, then release it.
 * Used by cmd_DeviceReset to hard-reboot an ESC. */
void esc_io_pulse_low(uint32_t ms);
