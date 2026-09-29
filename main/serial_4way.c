#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "esc_io.h"
#include "host_link.h"
#include "blheli_bootloader.h"
#include "led.h"
#include "serial_4way.h"

/* Frame delimiters */
#define cmd_Remote_Escape       0x2E   /* '.' device -> host */
#define cmd_Local_Escape        0x2F   /* '/' host -> device */

/* Commands */
#define cmd_InterfaceTestAlive  0x30
#define cmd_ProtocolGetVersion  0x31
#define cmd_InterfaceGetName    0x32
#define cmd_InterfaceGetVersion 0x33
#define cmd_InterfaceExit       0x34
#define cmd_DeviceReset         0x35
#define cmd_DeviceInitFlash     0x37
#define cmd_DeviceEraseAll      0x38
#define cmd_DevicePageErase     0x39
#define cmd_DeviceRead          0x3A
#define cmd_DeviceWrite         0x3B
#define cmd_DeviceC2CK_LOW      0x3C
#define cmd_DeviceReadEEprom    0x3D
#define cmd_DeviceWriteEEprom   0x3E
#define cmd_InterfaceSetMode    0x3F
#define cmd_DeviceVerify        0x40

/* ACK codes */
#define ACK_OK                  0x00
#define ACK_I_INVALID_CMD       0x02
#define ACK_I_INVALID_CRC       0x03
#define ACK_I_VERIFY_ERROR      0x04
#define ACK_I_INVALID_CHANNEL   0x08
#define ACK_I_INVALID_PARAM     0x09
#define ACK_D_GENERAL_ERROR     0x0F

#define INTF_MODE_IDX           3

#define SERIAL_4WAY_VERSION \
    ((uint16_t)((ESC4W_VER_MAIN * 1000) + (ESC4W_VER_SUB_1 * 100) + ESC4W_VER_SUB_2))
#define SERIAL_4WAY_VERSION_HI  ((uint8_t)(SERIAL_4WAY_VERSION / 100))
#define SERIAL_4WAY_VERSION_LO  ((uint8_t)(SERIAL_4WAY_VERSION % 100))

/* Device family matching, verbatim from upstream.
 * ARM covers BLHeli_32 and AM32: signature high in 0x01..0x8F, low
 * always 0x06 (e.g. 0x1F06 STM32F051, 0x3506 F421, 0x2B06 G071). */
#define ATMEL_DEVICE_MATCH(sig)  ((sig) == 0x9307 || (sig) == 0x930A || \
                                  (sig) == 0x930F || (sig) == 0x940B)
#define SILABS_DEVICE_MATCH(sig) ((sig) > 0xE800 && (sig) < 0xF900)
#define ARM_DEVICE_MATCH(hi, lo) ((hi) > 0x00 && (hi) < 0x90 && (lo) == 0x06)

static uint8_t s_esc_count;
static uint8_t s_selected_esc;
static uint8_t s_interface_mode;

static uint8_t s_param_buf[ESC4W_PARAM_BUF_SIZE];
static uint8_t s_reply_buf[ESC4W_REPLY_BUF_SIZE];

/* ------------------------------------------------------------------ */
/* CRC16-XMODEM — host framing only                                    */
/* ------------------------------------------------------------------ */

static uint16_t crc_xmodem_update(uint16_t crc, uint8_t data)
{
    crc ^= (uint16_t)data << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000) {
            crc = (uint16_t)((crc << 1) ^ 0x1021);
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Setup / teardown                                                    */
/* ------------------------------------------------------------------ */

uint8_t esc4way_init(void)
{
    s_esc_count = esc_io_init();
    s_selected_esc = 0;
    s_interface_mode = imARM_BLB;
    bl_set_disconnected();
    bl_device_info[2] = 0;
    bl_device_info[INTF_MODE_IDX] = 0;

    esc4way_log_levels("passthrough:");
    return s_esc_count;
}

void esc4way_log_levels(const char *prefix)
{
    /* Idle level of every parked pad. A bootloader only waits for us
     * while its signal line reads high; a 0 here means the ESC side is
     * pulling the line down harder than our pull-up holds it up. */
    char levels[96];
    int n = 0;
    for (uint8_t i = 0; i < ESC4W_ESC_COUNT && n < (int)sizeof(levels) - 16; i++) {
        n += snprintf(levels + n, sizeof(levels) - (size_t)n, " GPIO%d=%d",
                      esc_io_pin(i), esc_io_level(i));
    }
    host_log("%s %u ESCs, idle levels:%s", prefix, ESC4W_ESC_COUNT, levels);
}

static void esc4way_release(void)
{
    esc_io_release();
    bl_set_disconnected();
}

/* Try each supported bootloader family, up to ESC4W_CONNECT_RETRIES. */
static bool esc4way_connect(void)
{
    for (uint8_t attempt = 0; attempt < ESC4W_CONNECT_RETRIES; attempt++) {
        if (!bl_connect()) {
            continue;
        }
        uint16_t sig = bl_signature();
        if (SILABS_DEVICE_MATCH(sig)) {
            s_interface_mode = imSIL_BLB;
            return true;
        }
        if (ATMEL_DEVICE_MATCH(sig)) {
            s_interface_mode = imATM_BLB;
            return true;
        }
        if (ARM_DEVICE_MATCH(bl_device_info[1], bl_device_info[0])) {
            s_interface_mode = imARM_BLB;
            return true;
        }
        host_log("  bootloader answered, unknown signature %04X", sig);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Frame reception                                                     */
/* ------------------------------------------------------------------ */

static uint16_t s_crc_in;

static bool read_byte_crc(uint8_t *data, uint32_t timeout_us)
{
    if (!host_read_byte(data, timeout_us)) {
        return true;   /* timed out */
    }
    s_crc_in = crc_xmodem_update(s_crc_in, *data);
    return false;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

void esc4way_process(void)
{
    bool exit_scheduled = false;

    for (;;) {
        uint8_t esc, cmd = 0, in_param_len = 0, ack_out;
        uint8_t addr_hi = 0, addr_lo = 0;
        uint16_t crc_check = 0;
        uint8_t dummy[2] = { 0, 0 };
        const uint8_t *out_param = dummy;
        uint16_t out_param_len = 1;
        bool timed_out = false;
        bl_mem_t mem;

        /* Hunt for the start-of-frame byte. No timeout here: upstream
         * sits on this read indefinitely and so does BLHeliSuite32. */
        do {
            s_crc_in = 0;
            read_byte_crc(&esc, 0);
        } while (esc != cmd_Local_Escape);

        timed_out = read_byte_crc(&cmd, ESC4W_CMD_TIMEOUT_US) ||
                    read_byte_crc(&addr_hi, ESC4W_ARG_TIMEOUT_US) ||
                    read_byte_crc(&addr_lo, ESC4W_ARG_TIMEOUT_US) ||
                    read_byte_crc(&in_param_len, ESC4W_ARG_TIMEOUT_US);

        if (!timed_out) {
            uint16_t count = in_param_len ? in_param_len : 256;
            for (uint16_t i = 0; i < count && !timed_out; i++) {
                timed_out = read_byte_crc(&s_param_buf[i], ESC4W_DAT_TIMEOUT_US);
            }
            /* CRC arrives high byte first and is not itself CRC'd. */
            uint8_t crc_hi = 0, crc_lo = 0;
            if (!timed_out) {
                timed_out = !host_read_byte(&crc_hi, ESC4W_CRC_TIMEOUT_US);
            }
            if (!timed_out) {
                timed_out = !host_read_byte(&crc_lo, ESC4W_CRC_TIMEOUT_US);
            }
            crc_check = (uint16_t)((crc_hi << 8) | crc_lo);
        }

        ack_out = (!timed_out && crc_check == s_crc_in) ? ACK_OK
                                                        : ACK_I_INVALID_CRC;
        host_log("4w cmd %02X addr %02X%02X len %u%s", cmd, addr_hi, addr_lo,
                 in_param_len ? in_param_len : 256,
                 timed_out ? " TIMEOUT" : (ack_out != ACK_OK ? " BAD CRC" : ""));

        mem.addr_hi = addr_hi;
        mem.addr_lo = addr_lo;
        mem.ptr = s_param_buf;
        mem.num_bytes = 0;

        if (ack_out == ACK_OK) {
            /* Flash the LED for anything that moves ESC memory. One GPIO
             * write, so it cannot disturb the one-wire timing. */
            switch (cmd) {
            case cmd_DevicePageErase:
            case cmd_DeviceRead:
            case cmd_DeviceWrite:
            case cmd_DeviceReadEEprom:
            case cmd_DeviceWriteEEprom:
            case cmd_DeviceVerify:
                led_activity();
                break;
            default:
                break;
            }

            switch (cmd) {

            /* ---------------- interface ---------------- */

            case cmd_InterfaceTestAlive:
                if (bl_is_connected()) {
                    switch (s_interface_mode) {
                    case imSIL_BLB:
                    case imATM_BLB:
                    case imARM_BLB:
                        if (!bl_keep_alive()) {
                            ack_out = ACK_D_GENERAL_ERROR;
                        }
                        break;
                    default:
                        ack_out = ACK_D_GENERAL_ERROR;
                        break;
                    }
                    if (ack_out != ACK_OK) {
                        bl_set_disconnected();
                    }
                }
                break;

            case cmd_ProtocolGetVersion:
                dummy[0] = ESC4W_PROTOCOL_VER;
                break;

            case cmd_InterfaceGetName:
                out_param_len = (uint16_t)strlen(ESC4W_INTERFACE_NAME_STR);
                out_param = (const uint8_t *)ESC4W_INTERFACE_NAME_STR;
                break;

            case cmd_InterfaceGetVersion:
                out_param_len = 2;
                dummy[0] = SERIAL_4WAY_VERSION_HI;
                dummy[1] = SERIAL_4WAY_VERSION_LO;
                break;

            case cmd_InterfaceExit:
                exit_scheduled = true;
                break;

            case cmd_InterfaceSetMode:
                if (s_param_buf[0] == imSIL_BLB ||
                    s_param_buf[0] == imATM_BLB ||
                    s_param_buf[0] == imARM_BLB) {
                    s_interface_mode = s_param_buf[0];
                } else {
                    /* imSK (SimonK/STK500v2) is not implemented here. */
                    ack_out = ACK_I_INVALID_PARAM;
                }
                break;

            /* ---------------- device ---------------- */

            case cmd_DeviceReset: {
                bool reboot_esc = false;
                if (s_param_buf[0] < s_esc_count) {
                    s_selected_esc = s_param_buf[0];
                    esc_io_select(s_selected_esc);
                    /* Upstream reads ParamBuf[1] unconditionally, which
                     * is a stale byte when the host sends only the ESC
                     * index — as both ESC Configurator and the AM32
                     * Configurator do. Same meaning, without the read of
                     * a parameter that was never sent. */
                    if (in_param_len >= 2 && s_param_buf[1] != 0) {
                        reboot_esc = true;
                    }
                } else {
                    ack_out = ACK_I_INVALID_CHANNEL;
                    break;
                }
                switch (s_interface_mode) {
                case imSIL_BLB:
                case imATM_BLB:
                case imARM_BLB:
                    bl_restart_bootloader();
                    if (reboot_esc) {
                        esc_io_pulse_low(ESC4W_RESET_PULSE_MS);
                    }
                    break;
                default:
                    break;
                }
                bl_set_disconnected();
                break;
            }

            case cmd_DeviceInitFlash:
                bl_set_disconnected();
                if (s_param_buf[0] < s_esc_count) {
                    s_selected_esc = s_param_buf[0];
                    esc_io_select(s_selected_esc);
                } else {
                    ack_out = ACK_I_INVALID_CHANNEL;
                    break;
                }
                out_param_len = 4;
                out_param = bl_device_info;
                host_log("  init ESC %u (GPIO%d), line %d before connect",
                         s_selected_esc + 1, esc_io_pin(s_selected_esc),
                         esc_io_level(s_selected_esc));
                if (esc4way_connect()) {
                    bl_device_info[INTF_MODE_IDX] = s_interface_mode;
                    host_log("  connected: signature %04X, mode %u",
                             bl_signature(), s_interface_mode);
                } else {
                    bl_set_disconnected();
                    ack_out = ACK_D_GENERAL_ERROR;
                }
                break;

            case cmd_DevicePageErase:
                switch (s_interface_mode) {
                case imSIL_BLB:
                case imARM_BLB:
                    dummy[0] = s_param_buf[0];
                    /* ARM pages are 1024 bytes, SiLabs 512. */
                    mem.addr_hi = (s_interface_mode == imARM_BLB)
                                      ? (uint8_t)(dummy[0] << 2)
                                      : (uint8_t)(dummy[0] << 1);
                    mem.addr_lo = 0;
                    if (!bl_page_erase(&mem)) {
                        ack_out = ACK_D_GENERAL_ERROR;
                    }
                    break;
                default:
                    ack_out = ACK_I_INVALID_CMD;
                    break;
                }
                break;

            case cmd_DeviceRead:
                mem.num_bytes = s_param_buf[0];
                switch (s_interface_mode) {
                case imSIL_BLB:
                case imATM_BLB:
                case imARM_BLB:
                    if (!bl_read_flash(s_interface_mode, &mem)) {
                        ack_out = ACK_D_GENERAL_ERROR;
                    }
                    break;
                default:
                    ack_out = ACK_I_INVALID_CMD;
                    break;
                }
                if (ack_out == ACK_OK) {
                    out_param_len = mem.num_bytes ? mem.num_bytes : 256;
                    out_param = s_param_buf;
                    /* First bytes only: enough for the AM32 settings
                     * header (boot byte, layout, bootloader, fw major,
                     * fw minor, name...) without flooding the trace. */
                    host_log_hex("   data:", s_param_buf,
                                 out_param_len < 24 ? out_param_len : 24);
                }
                break;

            case cmd_DeviceReadEEprom:
                mem.num_bytes = s_param_buf[0];
                switch (s_interface_mode) {
                case imATM_BLB:
                    if (!bl_read_eeprom(&mem)) {
                        ack_out = ACK_D_GENERAL_ERROR;
                    }
                    break;
                default:
                    ack_out = ACK_I_INVALID_CMD;
                    break;
                }
                if (ack_out == ACK_OK) {
                    out_param_len = mem.num_bytes ? mem.num_bytes : 256;
                    out_param = s_param_buf;
                }
                break;

            case cmd_DeviceWrite:
                mem.num_bytes = in_param_len;
                switch (s_interface_mode) {
                case imSIL_BLB:
                case imATM_BLB:
                case imARM_BLB:
                    if (!bl_write_flash(&mem)) {
                        ack_out = ACK_D_GENERAL_ERROR;
                    }
                    break;
                default:
                    ack_out = ACK_I_INVALID_CMD;
                    break;
                }
                break;

            case cmd_DeviceWriteEEprom:
                mem.num_bytes = in_param_len;
                ack_out = ACK_D_GENERAL_ERROR;
                switch (s_interface_mode) {
                case imSIL_BLB:
                    ack_out = ACK_I_INVALID_CMD;
                    break;
                case imATM_BLB:
                    if (bl_write_eeprom(&mem)) {
                        ack_out = ACK_OK;
                    }
                    break;
                default:
                    break;
                }
                break;

            case cmd_DeviceVerify:
                if (s_interface_mode == imARM_BLB) {
                    mem.num_bytes = in_param_len;
                    switch (bl_verify_flash(&mem)) {
                    case brSUCCESS:
                        ack_out = ACK_OK;
                        break;
                    case brERRORVERIFY:
                        ack_out = ACK_I_VERIFY_ERROR;
                        break;
                    default:
                        ack_out = ACK_D_GENERAL_ERROR;
                        break;
                    }
                } else {
                    ack_out = ACK_I_INVALID_CMD;
                }
                break;

            /* cmd_DeviceEraseAll and cmd_DeviceC2CK_LOW belong to the
             * SimonK/C2 paths, which this port does not implement. */
            default:
                ack_out = ACK_I_INVALID_CMD;
                break;
            }
        }

        /* ---------------- reply ---------------- */

        uint16_t crc_out = 0;
        uint16_t n = 0;

        #define PUT_CRC(b) do {                                   \
            uint8_t _b = (uint8_t)(b);                            \
            s_reply_buf[n++] = _b;                                \
            crc_out = crc_xmodem_update(crc_out, _b);             \
        } while (0)

        PUT_CRC(cmd_Remote_Escape);
        PUT_CRC(cmd);
        PUT_CRC(mem.addr_hi);
        PUT_CRC(mem.addr_lo);
        PUT_CRC(out_param_len & 0xFF);   /* 256 is sent as 0 */

        for (uint16_t i = 0; i < out_param_len; i++) {
            PUT_CRC(out_param[i]);
        }
        PUT_CRC(ack_out);

        #undef PUT_CRC

        s_reply_buf[n++] = (uint8_t)(crc_out >> 8);
        s_reply_buf[n++] = (uint8_t)(crc_out & 0xFF);

        host_write(s_reply_buf, n);
        host_flush();
        host_log("   -> ack %02X, %u bytes", ack_out, out_param_len);

        if (exit_scheduled) {
            esc4way_release();
            return;
        }
    }
}
