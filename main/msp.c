#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "config.h"
#include "host_link.h"
#include "serial_4way.h"
#include "msp.h"

/* Commands we answer */
#define MSP_API_VERSION          1
#define MSP_FC_VARIANT           2
#define MSP_FC_VERSION           3
#define MSP_BOARD_INFO           4
#define MSP_BUILD_INFO           5
#define MSP_NAME                 10
#define MSP_FEATURE_CONFIG       36
#define MSP_REBOOT               68
#define MSP_STATUS               101
#define MSP_MOTOR                104
#define MSP_MOTOR_3D_CONFIG      124
#define MSP_BATTERY_STATE        130
#define MSP_MOTOR_CONFIG         131
#define MSP_STATUS_EX            150
#define MSP_UID                  160
#define MSP_SET_MOTOR            214
#define MSP_SET_PASSTHROUGH      245

#define MSP_PASSTHROUGH_ESC_4WAY 0xFF

#define MSP_MAX_PAYLOAD          256

/* ------------------------------------------------------------------ */
/* Response assembly                                                   */
/* ------------------------------------------------------------------ */

static uint8_t  s_out[MSP_MAX_PAYLOAD];
static uint16_t s_out_len;
static bool     s_passthrough_pending;
static bool     s_reboot_pending;

static void out_reset(void)      { s_out_len = 0; }
static void out_u8(uint8_t v)    { if (s_out_len < MSP_MAX_PAYLOAD) s_out[s_out_len++] = v; }
static void out_u16(uint16_t v)  { out_u8(v & 0xFF); out_u8(v >> 8); }
static void out_u32(uint32_t v)  { out_u16(v & 0xFFFF); out_u16(v >> 16); }

static void out_data(const void *p, uint16_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (uint16_t i = 0; i < n; i++) {
        out_u8(b[i]);
    }
}

/* Length-prefixed string, as sbufWritePString(). */
static void out_pstring(const char *s)
{
    uint8_t n = (uint8_t)strlen(s);
    out_u8(n);
    out_data(s, n);
}

static uint8_t crc8_dvb_s2(uint8_t crc, uint8_t a)
{
    crc ^= a;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
    }
    return crc;
}

static void send_reply_v1(uint16_t cmd, bool ok)
{
    uint8_t hdr[5];
    uint8_t crc = 0;

    hdr[0] = '$';
    hdr[1] = 'M';
    hdr[2] = ok ? '>' : '!';
    hdr[3] = (uint8_t)s_out_len;    /* v1 payloads are capped at 255 */
    hdr[4] = (uint8_t)cmd;
    crc ^= hdr[3];
    crc ^= hdr[4];
    for (uint16_t i = 0; i < s_out_len; i++) {
        crc ^= s_out[i];
    }

    host_write(hdr, sizeof(hdr));
    host_write(s_out, s_out_len);
    host_write(&crc, 1);
    host_flush();
}

static void send_reply_v2(uint16_t cmd, bool ok)
{
    uint8_t hdr[8];
    uint8_t crc = 0;

    hdr[0] = '$';
    hdr[1] = 'X';
    hdr[2] = ok ? '>' : '!';
    hdr[3] = 0;                        /* flag */
    hdr[4] = (uint8_t)(cmd & 0xFF);
    hdr[5] = (uint8_t)(cmd >> 8);
    hdr[6] = (uint8_t)(s_out_len & 0xFF);
    hdr[7] = (uint8_t)(s_out_len >> 8);
    for (int i = 3; i < 8; i++) {
        crc = crc8_dvb_s2(crc, hdr[i]);
    }
    for (uint16_t i = 0; i < s_out_len; i++) {
        crc = crc8_dvb_s2(crc, s_out[i]);
    }

    host_write(hdr, sizeof(hdr));
    host_write(s_out, s_out_len);
    host_write(&crc, 1);
    host_flush();
}

/* ------------------------------------------------------------------ */
/* Command handlers                                                    */
/* ------------------------------------------------------------------ */

static void fill_uid(void)
{
#if ESC4W_MSP_UID_FROM_MAC
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    out_data(mac, 6);
    out_data(mac, 6);
#else
    static const uint8_t uid[12] = ESC4W_MSP_UID_FALLBACK;
    out_data(uid, sizeof(uid));
#endif
}

static bool msp_handle(uint16_t cmd, const uint8_t *payload, uint16_t len)
{
    out_reset();

    switch (cmd) {

    case MSP_API_VERSION:
        out_u8(ESC4W_MSP_PROTOCOL_VERSION);
        out_u8(ESC4W_MSP_API_MAJOR);
        out_u8(ESC4W_MSP_API_MINOR);
        return true;

    case MSP_FC_VARIANT:
        out_data(ESC4W_MSP_FC_VARIANT, 4);
        return true;

    case MSP_FC_VERSION:
        out_u8(ESC4W_MSP_FC_VER_MAJOR);
        out_u8(ESC4W_MSP_FC_VER_MINOR);
        out_u8(ESC4W_MSP_FC_VER_PATCH);
        return true;

    case MSP_BOARD_INFO: {
        static const uint8_t signature[32] = { 0 };
        out_data(ESC4W_MSP_BOARD_ID, 4);
        out_u16(0);                       /* hardware revision */
        out_u8(0);                        /* 0 = FC, no OSD */
        out_u8(1 << 0);                   /* target capabilities: has VCP */
        out_pstring(ESC4W_MSP_TARGET_NAME);
        out_pstring(ESC4W_MSP_BOARD_NAME);
        out_pstring(ESC4W_MSP_MANUFACTURER_ID);
        out_data(signature, sizeof(signature));
        out_u8(255);                      /* MCU type: provided by name */
        out_u8(2);                        /* configuration state: configured */
        out_u16(8000);                    /* gyro sample rate, informational */
        out_u32(0);                       /* configuration problems */
        out_u8(0);                        /* SPI device count */
        out_u8(0);                        /* I2C device count */
        return true;
    }

    case MSP_BUILD_INFO:
        out_data(ESC4W_MSP_BUILD_DATE, 11);
        out_data(ESC4W_MSP_BUILD_TIME, 8);
        out_data(ESC4W_MSP_GIT_REVISION, 7);
        return true;

    case MSP_NAME:
        out_data(ESC4W_MSP_BOARD_NAME, strlen(ESC4W_MSP_BOARD_NAME));
        return true;

    case MSP_FEATURE_CONFIG:
        out_u32(0);
        return true;

    case MSP_STATUS:
    case MSP_STATUS_EX:
        out_u16(1000);      /* cycle time, us */
        out_u16(0);         /* I2C error count */
        out_u16(0);         /* active sensors */
        out_u32(0);         /* flight mode flags */
        out_u8(0);          /* current PID profile */
        out_u16(0);         /* average system load, % */
        out_u8(1);          /* PID profile count */
        out_u8(0);          /* current rate profile */
        out_u8(0);          /* extra flight mode flag byte count */
        out_u8(4);          /* arming disable flag count */
        out_u32(0);         /* arming disable flags */
        out_u8(0);          /* reboot required */
        return true;

    case MSP_MOTOR:
        /* Always eight slots, as Betaflight does. */
        for (int i = 0; i < 8; i++) {
            out_u16(i < ESC4W_MSP_MOTOR_COUNT ? 1000 : 0);
        }
        return true;

    case MSP_MOTOR_CONFIG:
        out_u16(1070);                      /* min throttle */
        out_u16(2000);                      /* max throttle */
        out_u16(1000);                      /* min command */
        out_u8(ESC4W_MSP_MOTOR_COUNT);
        out_u8(ESC4W_MSP_MOTOR_POLES);
        out_u8(0);                          /* dshot telemetry */
        out_u8(0);                          /* ESC sensor */
        return true;

    case MSP_MOTOR_3D_CONFIG:
        out_u16(1406);
        out_u16(1514);
        out_u16(1460);
        return true;

    case MSP_BATTERY_STATE:
        out_u8(0);          /* cell count */
        out_u16(0);         /* capacity, mAh */
        out_u8(0);          /* legacy voltage, 0.1 V */
        out_u16(0);         /* mAh drawn */
        out_u16(0);         /* current, 0.01 A */
        out_u8(0);          /* battery state: OK */
        out_u16(0);         /* voltage, 0.01 V */
        return true;

    case MSP_UID:
        fill_uid();
        return true;

    case MSP_SET_MOTOR:
        /* No motors to drive — accept and ignore so the configurator's
         * motor test does not error out. */
        return true;

    case MSP_REBOOT:
        out_u8(0);          /* reboot mode: firmware */
        s_reboot_pending = true;
        return true;

    case MSP_SET_PASSTHROUGH: {
        uint8_t mode = MSP_PASSTHROUGH_ESC_4WAY;
        if (len >= 1) {
            mode = payload[0];            /* payload[1] is the argument */
        }
        if (mode == MSP_PASSTHROUGH_ESC_4WAY) {
            /* Reply with the number of ESC outputs, then switch the
             * port out of MSP and into the 4-way protocol. */
            out_u8(esc4way_init());
            s_passthrough_pending = true;
        } else {
            out_u8(0);
        }
        return true;
    }

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Frame parser                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    ST_IDLE,
    ST_HEADER_START,
    ST_HEADER_M,
    ST_HEADER_X,
    ST_V1_LEN,
    ST_V1_CMD,
    ST_V1_PAYLOAD,
    ST_V1_CRC,
    ST_V2_FLAG,
    ST_V2_CMD_LO,
    ST_V2_CMD_HI,
    ST_V2_LEN_LO,
    ST_V2_LEN_HI,
    ST_V2_PAYLOAD,
    ST_V2_CRC,
} msp_state_t;

static msp_state_t s_state;
static uint8_t     s_payload[MSP_MAX_PAYLOAD];
static uint16_t    s_cmd;
static uint16_t    s_len;
static uint16_t    s_idx;
static uint8_t     s_crc;
static bool        s_is_v2;

void msp_init(void)
{
    s_state = ST_IDLE;
    s_passthrough_pending = false;
    s_reboot_pending = false;
}

bool msp_take_passthrough_request(void)
{
    if (s_reboot_pending) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    if (s_passthrough_pending) {
        s_passthrough_pending = false;
        return true;
    }
    return false;
}

static void dispatch(void)
{
    bool ok = msp_handle(s_cmd, s_payload, s_len);
    if (!ok) {
        out_reset();
    }
    if (s_is_v2) {
        send_reply_v2(s_cmd, ok);
    } else {
        send_reply_v1(s_cmd, ok);
    }
    /* After the reply, so tracing never delays it. */
    host_log("msp%s %u -> %s, %u bytes", s_is_v2 ? " v2" : "", s_cmd,
             ok ? "ok" : "unhandled", s_out_len);
}

void msp_process_byte(uint8_t b)
{
    switch (s_state) {

    case ST_IDLE:
        if (b == '$') {
            s_state = ST_HEADER_START;
        }
        break;

    case ST_HEADER_START:
        if (b == 'M') {
            s_state = ST_HEADER_M;
        } else if (b == 'X') {
            s_state = ST_HEADER_X;
        } else {
            s_state = ST_IDLE;
        }
        break;

    case ST_HEADER_M:
        s_is_v2 = false;
        s_state = (b == '<') ? ST_V1_LEN : ST_IDLE;
        break;

    case ST_HEADER_X:
        s_is_v2 = true;
        s_state = (b == '<') ? ST_V2_FLAG : ST_IDLE;
        break;

    /* ---- MSP v1 ---- */

    case ST_V1_LEN:
        s_len = b;
        s_crc = b;
        s_state = ST_V1_CMD;
        break;

    case ST_V1_CMD:
        s_cmd = b;
        s_crc ^= b;
        s_idx = 0;
        s_state = s_len ? ST_V1_PAYLOAD : ST_V1_CRC;
        break;

    case ST_V1_PAYLOAD:
        if (s_idx < MSP_MAX_PAYLOAD) {
            s_payload[s_idx] = b;
        }
        s_crc ^= b;
        if (++s_idx >= s_len) {
            s_state = ST_V1_CRC;
        }
        break;

    case ST_V1_CRC:
        if (b == s_crc) {
            dispatch();
        } else {
            host_log("msp %u: bad checksum, dropped", s_cmd);
        }
        s_state = ST_IDLE;
        break;

    /* ---- MSP v2 ---- */

    case ST_V2_FLAG:
        s_crc = crc8_dvb_s2(0, b);
        s_state = ST_V2_CMD_LO;
        break;

    case ST_V2_CMD_LO:
        s_cmd = b;
        s_crc = crc8_dvb_s2(s_crc, b);
        s_state = ST_V2_CMD_HI;
        break;

    case ST_V2_CMD_HI:
        s_cmd |= (uint16_t)b << 8;
        s_crc = crc8_dvb_s2(s_crc, b);
        s_state = ST_V2_LEN_LO;
        break;

    case ST_V2_LEN_LO:
        s_len = b;
        s_crc = crc8_dvb_s2(s_crc, b);
        s_state = ST_V2_LEN_HI;
        break;

    case ST_V2_LEN_HI:
        s_len |= (uint16_t)b << 8;
        s_crc = crc8_dvb_s2(s_crc, b);
        s_idx = 0;
        if (s_len > MSP_MAX_PAYLOAD) {
            s_state = ST_IDLE;      /* oversized, drop the frame */
        } else {
            s_state = s_len ? ST_V2_PAYLOAD : ST_V2_CRC;
        }
        break;

    case ST_V2_PAYLOAD:
        s_payload[s_idx] = b;
        s_crc = crc8_dvb_s2(s_crc, b);
        if (++s_idx >= s_len) {
            s_state = ST_V2_CRC;
        }
        break;

    case ST_V2_CRC:
        if (b == s_crc) {
            dispatch();
        } else {
            host_log("msp v2 %u: bad checksum, dropped", s_cmd);
        }
        s_state = ST_IDLE;
        break;
    }
}
