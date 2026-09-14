/*
 * host_link.h — byte transport to the configurator.
 *
 * Two backends, selected by ESC4W_USB_TINYUSB in config.h: TinyUSB over
 * the ESP32-S3's USB-OTG peripheral (GPIO19/20), or the ESP32-C3's
 * native USB Serial/JTAG (GPIO18/19). Either way the device is CDC — no
 * external USB-serial bridge and no CP2102 involved.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

void host_link_init(void);

/* Blocking read of a single byte.
 * timeout_us == 0 waits forever. Returns false on timeout. */
bool host_read_byte(uint8_t *b, uint32_t timeout_us);

/* Queue bytes for transmission. */
void host_write(const uint8_t *buf, uint16_t len);

/* Push everything queued out to the host. */
void host_flush(void);

/* Diagnostic trace (ESC4W_LOG_ENABLE, S3/TinyUSB backend only).
 *
 * Goes to a second CDC port on the same USB cable — on Linux the
 * protocol port is /dev/ttyACM0 and the trace is /dev/ttyACM1. Lines
 * are only written while a terminal holds the trace port open, never
 * block, and are dropped if its buffer is full, so tracing can neither
 * stall nor corrupt the protocol port. */
#if ESC4W_LOG_ENABLE && ESC4W_USB_TINYUSB
void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void host_log_hex(const char *prefix, const uint8_t *buf, uint16_t len);
/* True exactly once each time a terminal opens the trace port. */
bool host_log_just_opened(void);
#else
static inline bool host_log_just_opened(void) { return false; }
static inline void host_log(const char *fmt, ...) { (void)fmt; }
static inline void host_log_hex(const char *prefix, const uint8_t *buf,
                                uint16_t len)
{
    (void)prefix; (void)buf; (void)len;
}
#endif
