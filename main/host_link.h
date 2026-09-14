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

void host_link_init(void);

/* Blocking read of a single byte.
 * timeout_us == 0 waits forever. Returns false on timeout. */
bool host_read_byte(uint8_t *b, uint32_t timeout_us);

/* Queue bytes for transmission. */
void host_write(const uint8_t *buf, uint16_t len);

/* Push everything queued out to the host. */
void host_flush(void);
