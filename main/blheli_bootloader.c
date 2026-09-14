#include <string.h>

#include "config.h"
#include "esc_io.h"
#include "host_link.h"
#include "blheli_bootloader.h"

/* Bootloader commands */
#define RestartBootloader     0
#define ExitBootloader        1

#define CMD_RUN               0x00
#define CMD_PROG_FLASH        0x01
#define CMD_ERASE_FLASH       0x02
#define CMD_READ_FLASH_SIL    0x03
#define CMD_VERIFY_FLASH      0x03
#define CMD_VERIFY_FLASH_ARM  0x04
#define CMD_READ_EEPROM       0x04
#define CMD_PROG_EEPROM       0x05
#define CMD_READ_SRAM         0x06
#define CMD_READ_FLASH_ATM    0x07
#define CMD_KEEP_ALIVE        0xFD
#define CMD_SET_ADDRESS       0xFF
#define CMD_SET_BUFFER        0xFE

/* Interface modes, shared with serial_4way.c */
#define imC2        0
#define imSIL_BLB   1
#define imATM_BLB   2
#define imSK        3
#define imARM_BLB   4

uint8_t bl_device_info[4] = { 0, 0, 0, 0 };

static uint16_t s_crc;

/* Diagnostics: what the last bl_read() actually got off the wire. */
static uint16_t s_rx_got;   /* payload bytes that arrived */
static uint8_t  s_rx_ack;   /* trailing ACK, brNONE if it never came */

/* Betaflight's BL_GetACK(n) is n iterations of a start-bit timeout. */
static inline uint32_t ack_ms(uint32_t count)
{
    uint32_t ms = count * ESC4W_ACK_TICK_MS;
    return ms < ESC4W_ACK_MIN_TIMEOUT_MS ? ESC4W_ACK_MIN_TIMEOUT_MS : ms;
}

/* CRC16-IBM / "Modbus" polynomial, reflected, init 0. */
static void crc_byte(uint8_t b)
{
    for (uint8_t i = 0; i < 8; i++) {
        if (((b & 0x01) ^ (s_crc & 0x0001)) != 0) {
            s_crc = (uint16_t)((s_crc >> 1) ^ 0xA001);
        } else {
            s_crc >>= 1;
        }
        b >>= 1;
    }
}

/* ------------------------------------------------------------------ */
/* Framed send / receive                                               */
/* ------------------------------------------------------------------ */

/* The handshake runs without CRC; every transaction after a successful
 * connect carries one. bl_is_connected() decides, exactly as upstream. */
static uint16_t bl_send(const uint8_t *buf, uint16_t len)
{
    uint8_t frame[ESC4W_PARAM_BUF_SIZE + 4];
    uint16_t n = 0;

    s_crc = 0;
    for (uint16_t i = 0; i < len && n < sizeof(frame); i++) {
        frame[n++] = buf[i];
        crc_byte(buf[i]);
    }
    if (bl_is_connected() && (size_t)(n + 2) <= sizeof(frame)) {
        frame[n++] = (uint8_t)(s_crc & 0xFF);
        frame[n++] = (uint8_t)(s_crc >> 8);
    }
    return esc_io_write(frame, n);
}

static bool bl_read(uint8_t *buf, uint16_t len)
{
    uint8_t last_ack = brNONE;
    uint8_t trailer[3];

    s_crc = 0;
    s_rx_ack = brNONE;

    s_rx_got = esc_io_read(buf, len, ESC4W_BYTE_TIMEOUT_MS);
    if (s_rx_got != len) {
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        crc_byte(buf[i]);
    }

    if (bl_is_connected()) {
        /* CRC low, CRC high, ACK */
        if (esc_io_read(trailer, 3, ESC4W_BYTE_TIMEOUT_MS) != 3) {
            return false;
        }
        uint16_t rx_crc = (uint16_t)(trailer[0] | (trailer[1] << 8));
        last_ack = (rx_crc == s_crc) ? trailer[2] : brERRORCRC;
    } else {
        if (esc_io_read(&last_ack, 1, ESC4W_BYTE_TIMEOUT_MS) != 1) {
            return false;
        }
    }

    s_rx_ack = last_ack;
    return last_ack == brSUCCESS;
}

static uint8_t bl_get_ack(uint32_t count)
{
    uint8_t ack = brNONE;
    if (esc_io_read(&ack, 1, ack_ms(count)) != 1) {
        return brNONE;
    }
    return ack;
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

bool bl_connect(void)
{
    /* 12 leading zero bytes of line sync, then the literal boot
     * sequence — 21 bytes, exactly BL_ConnectEx()'s BootInit[]. */
    static const uint8_t boot_init[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x0D, 'B', 'L', 'H', 'e', 'l', 'i', 0xF4, 0x7D
    };
    uint8_t boot_info[8];

    /* Clear the connection state first, as upstream does. Otherwise a
     * device that answers "471x" with an unrecognised signature leaves
     * bl_is_connected() true, and the remaining retries in
     * esc4way_connect() send this handshake with a CRC appended and
     * expect one back — which no bootloader will answer. */
    memset(bl_device_info, 0, sizeof(bl_device_info));

    uint16_t echo = bl_send(boot_init, sizeof(boot_init));
    bool ok = bl_read(boot_info, sizeof(boot_info));

    /* echo < sent: our frame never made it onto the pad.
     * echo == sent, reply 0: we transmit fine, nothing answers.
     * reply > 0 but not "471": something answers, garbled. */
    host_log("  connect: sent %u echo %u, reply %u/%u, ack %02X",
             (unsigned)sizeof(boot_init), echo, s_rx_got,
             (unsigned)sizeof(boot_info), s_rx_ack);
    if (s_rx_got > 0) {
        host_log_hex("  reply:", boot_info, s_rx_got);
    }
    if (!ok) {
        return false;
    }

    /* Expected reply: "471x" SIG_HI SIG_LO BOOTVER BOOTPAGES.
     * Only the first three characters are checked, as upstream does. */
    if (boot_info[0] != '4' || boot_info[1] != '7' || boot_info[2] != '1') {
        return false;
    }

    bl_device_info[2] = boot_info[3];
    bl_device_info[1] = boot_info[4];   /* signature high */
    bl_device_info[0] = boot_info[5];   /* signature low  */
    return true;
}

bool bl_keep_alive(void)
{
    const uint8_t cmd[] = { CMD_KEEP_ALIVE, 0 };
    bl_send(cmd, sizeof(cmd));
    /* A live bootloader rejects this command — that rejection *is* the
     * proof of life. Anything else means the link is gone. */
    return bl_get_ack(1) == brERRORCOMMAND;
}

void bl_restart_bootloader(void)
{
    const uint8_t cmd[] = { RestartBootloader, 0 };
    bl_device_info[0] = 1;   /* force CRC on, matching upstream */
    bl_send(cmd, sizeof(cmd));
}

/* ------------------------------------------------------------------ */
/* Memory operations                                                   */
/* ------------------------------------------------------------------ */

static bool bl_set_address(bl_mem_t *mem)
{
    /* 0xFFFF is the "keep current address" sentinel. */
    if (mem->addr_hi == 0xFF && mem->addr_lo == 0xFF) {
        return true;
    }
    const uint8_t cmd[] = { CMD_SET_ADDRESS, 0, mem->addr_hi, mem->addr_lo };
    bl_send(cmd, sizeof(cmd));
    return bl_get_ack(2) == brSUCCESS;
}

static bool bl_set_buffer(bl_mem_t *mem)
{
    uint8_t cmd[4] = { CMD_SET_BUFFER, 0, 0, mem->num_bytes };
    if (mem->num_bytes == 0) {
        cmd[2] = 1;   /* 0 means 256 — set the high byte instead */
    }
    bl_send(cmd, sizeof(cmd));

    /* The bootloader must stay silent here; any byte is an error. */
    if (bl_get_ack(2) != brNONE) {
        return false;
    }

    uint16_t len = mem->num_bytes ? mem->num_bytes : 256;
    bl_send(mem->ptr, len);
    return bl_get_ack(40) == brSUCCESS;
}

static bool bl_read_a(uint8_t command, bl_mem_t *mem)
{
    if (!bl_set_address(mem)) {
        return false;
    }
    const uint8_t cmd[] = { command, mem->num_bytes };
    bl_send(cmd, sizeof(cmd));

    uint16_t len = mem->num_bytes ? mem->num_bytes : 256;
    if (!bl_read(mem->ptr, len)) {
        host_log("  read %02X%02X: reply %u/%u, ack %02X",
                 mem->addr_hi, mem->addr_lo, s_rx_got, len, s_rx_ack);
        return false;
    }
    return true;
}

static bool bl_write_a(uint8_t command, bl_mem_t *mem, uint32_t timeout_count)
{
    if (!bl_set_address(mem)) {
        return false;
    }
    if (!bl_set_buffer(mem)) {
        return false;
    }
    const uint8_t cmd[] = { command, 0x01 };
    bl_send(cmd, sizeof(cmd));
    return bl_get_ack(timeout_count) == brSUCCESS;
}

bool bl_read_flash(uint8_t interface_mode, bl_mem_t *mem)
{
    if (interface_mode == imATM_BLB) {
        return bl_read_a(CMD_READ_FLASH_ATM, mem);
    }
    return bl_read_a(CMD_READ_FLASH_SIL, mem);
}

bool bl_read_eeprom(bl_mem_t *mem)
{
    return bl_read_a(CMD_READ_EEPROM, mem);
}

bool bl_page_erase(bl_mem_t *mem)
{
    if (!bl_set_address(mem)) {
        return false;
    }
    const uint8_t cmd[] = { CMD_ERASE_FLASH, 0x01 };
    bl_send(cmd, sizeof(cmd));
    return bl_get_ack(3000 / ESC4W_ACK_TICK_MS) == brSUCCESS;
}

bool bl_write_eeprom(bl_mem_t *mem)
{
    return bl_write_a(CMD_PROG_EEPROM, mem, 3000 / ESC4W_ACK_TICK_MS);
}

bool bl_write_flash(bl_mem_t *mem)
{
    return bl_write_a(CMD_PROG_FLASH, mem, 500 / ESC4W_ACK_TICK_MS);
}

uint8_t bl_verify_flash(bl_mem_t *mem)
{
    if (!bl_set_address(mem)) {
        return brNONE;
    }
    if (!bl_set_buffer(mem)) {
        return brNONE;
    }
    const uint8_t cmd[] = { CMD_VERIFY_FLASH_ARM, 0x01 };
    bl_send(cmd, sizeof(cmd));
    /* bl_get_ack() counts start-bit timeouts, not milliseconds — 40 here
     * is upstream's BL_GetACK(40), i.e. 80 ms, same unit as the count in
     * bl_set_buffer() above. */
    return bl_get_ack(40);
}
