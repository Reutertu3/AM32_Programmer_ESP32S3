/*
 * blheli_bootloader.h — device-side bootloader protocol.
 *
 * Port of Betaflight src/main/io/serial_4way_avrootloader.c
 * (Hagen Reddmann's AVRootloader, as used by BLHeli_S, BLHeli_32 and
 * AM32). Byte transport is delegated to esc_io.
 *
 * Note the two different CRCs in this project:
 *   - this layer uses CRC16-IBM (poly 0xA001, reflected, sent LSB first)
 *   - the host framing in serial_4way.c uses CRC16-XMODEM, MSB first
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bootloader result codes */
#define brSUCCESS        0x30
#define brERRORVERIFY    0xC0
#define brERRORCOMMAND   0xC1
#define brERRORCRC       0xC2
#define brNONE           0xFF

/* Memory operation descriptor, equivalent to upstream ioMem_t. */
typedef struct {
    uint8_t  num_bytes;      /* 0 means 256 */
    uint8_t  addr_hi;
    uint8_t  addr_lo;
    uint8_t *ptr;
} bl_mem_t;

/* Global connection state, laid out exactly as the 4 bytes returned by
 * cmd_DeviceInitFlash:
 *   [0] signature low   [1] signature high
 *   [2] boot message 4th character   [3] interface mode
 */
extern uint8_t bl_device_info[4];

static inline bool bl_is_connected(void)
{
    return bl_device_info[0] > 0;
}

static inline void bl_set_disconnected(void)
{
    bl_device_info[0] = 0;
    bl_device_info[1] = 0;
}

static inline uint16_t bl_signature(void)
{
    return (uint16_t)((bl_device_info[1] << 8) | bl_device_info[0]);
}

/* Handshake. Fills bl_device_info[0..2]. Returns true on success. */
bool bl_connect(void);

bool bl_keep_alive(void);
void bl_restart_bootloader(void);

bool bl_page_erase(bl_mem_t *mem);
bool bl_read_flash(uint8_t interface_mode, bl_mem_t *mem);
bool bl_read_eeprom(bl_mem_t *mem);
bool bl_write_flash(bl_mem_t *mem);
bool bl_write_eeprom(bl_mem_t *mem);

/* Returns the raw bootloader ACK (brSUCCESS / brERRORVERIFY / ...) so
 * the caller can map it onto the 4-way ACK space. */
uint8_t bl_verify_flash(bl_mem_t *mem);
