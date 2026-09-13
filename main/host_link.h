/*
 * host_link.h — byte transport to the configurator.
 *
 * The ESP32-C3's native USB Serial/JTAG peripheral is used as a CDC
 * device on the fixed pins GPIO18 (D-) and GPIO19 (D+). No external
 * USB-serial bridge and no CP2102 involved.
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
