/*
 * config.h — all board- and behaviour-specific settings for the
 * ESP32-S3 / ESP32-C3 BLHeli/AM32 4-way USB programmer.
 *
 * Nothing outside this file should need editing for a new board.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* 0. Target selection                                                 */
/* ------------------------------------------------------------------ */

/* 1 = ESP32-S3: USB-OTG + TinyUSB, so the device can present any
 *     VID/PID. Required if you want tools that filter the port list by
 *     vendor ID (the AM32 Configurator does) to see the programmer.
 * 0 = ESP32-C3: native USB Serial/JTAG. Its descriptors live in ROM and
 *     cannot be changed; the device always enumerates as 303a:1001.
 */
#define ESC4W_USB_TINYUSB            1

/* ------------------------------------------------------------------ */
/* 1. ESC signal pins                                                  */
/* ------------------------------------------------------------------ */

/* Number of ESC outputs reported to the configurator via
 * MSP_SET_PASSTHROUGH. Must match the length of ESC4W_ESC_PINS. */
#define ESC4W_ESC_COUNT              4
// #define ESC4W_ESC_COUNT              6

/* One GPIO per ESC signal line, in the order the configurator will
 * address them (index 0 = "ESC 1" in the UI).
 *
 * ESP32-S3 constraints — do not use:
 *   GPIO19 / GPIO20   USB D- / D+ (shared by OTG and Serial/JTAG)
 *   GPIO26..32        SPI flash
 *   GPIO33..37        Octal PSRAM/flash on N8R8, N16R8 and similar
 *   GPIO43 / GPIO44   default UART0 console (keep free)
 *   GPIO0, 3, 45, 46  strapping pins
 * GPIO22..25 do not exist on the S3.
 *
 * ESP32-C3 constraints — do not use:
 *   GPIO18 / GPIO19   native USB D- / D+
 *   GPIO11..17        SPI flash (GPIO11 = VDD_SPI on most modules)
 *   GPIO20 / GPIO21   default UART0 console
 *   GPIO2, 8, 9       strapping pins
 */
#if ESC4W_USB_TINYUSB
# define ESC4W_ESC_PINS             { 4, 5, 6, 7 }      /* ESP32-S3 */
// #define ESC4W_ESC_PINS               { 4, 5, 6, 7, 8, 9 }
#else
#  define ESC4W_ESC_PINS             { 3, 4, 5, 6 }      /* ESP32-C3 */
#endif

/* Optional activity LED. Set to -1 to disable.
 * Note: GPIO48 on the S3-DevKitC-1 and GPIO8 on many C3 devkits are
 * addressable WS2812s, not plain LEDs. */
#define ESC4W_LED_PIN                (21)
#define ESC4W_LED_ACTIVE_LOW         0

/* ------------------------------------------------------------------ */
/* 2. One-wire link to the ESC bootloader                              */
/* ------------------------------------------------------------------ */

/* Hardware UART used for the half-duplex link. Its TX and RX signals
 * are matrixed onto whichever ESC pin is currently selected.
 * UART0 is left alone so the debug console stays usable. */
#define ESC4W_UART_NUM               1

/* BLHeli/AM32 bootloader line rate. Betaflight bit-bangs 52 us/bit,
 * i.e. 19230 baud; 19200 is within tolerance. */
#define ESC4W_BL_BAUD                19200

/* Drive mode while transmitting.
 *   1 = push-pull (matches Betaflight's IOCFG_OUT_PP)
 *   0 = open-drain, relies on the pull-up for the rising edge
 * Push-pull is the safer default on longer leads. */
#define ESC4W_TX_PUSH_PULL           1

/* Enable the internal pull-up on the idle/receive line. The internal
 * pull-up is weak (~45 kOhm); an external 10 kOhm to 3V3 on each signal
 * line is recommended for anything over ~10 cm. */
#define ESC4W_RX_INTERNAL_PULLUP     1

/* ------------------------------------------------------------------ */
/* 3. Bootloader transaction timeouts                                  */
/* ------------------------------------------------------------------ */

/* Per-byte receive timeout. Betaflight's suart uses a 2 ms start-bit
 * timeout; 3 ms gives a little margin for FreeRTOS tick granularity. */
#define ESC4W_BYTE_TIMEOUT_MS        3

/* Betaflight expresses ACK waits as a count of start-bit timeouts.
 * ack_timeout_ms = count * ESC4W_ACK_TICK_MS, floored at MIN. */
#define ESC4W_ACK_TICK_MS            2
#define ESC4W_ACK_MIN_TIMEOUT_MS     5

/* Connect() attempts per cmd_DeviceInitFlash. Upstream uses 3. */
#define ESC4W_CONNECT_RETRIES        3

/* Low pulse length used by cmd_DeviceReset when the host asks for a
 * hard ESC reboot (address low byte == 1). */
#define ESC4W_RESET_PULSE_MS         300

/* ------------------------------------------------------------------ */
/* 4. Host framing timeouts (microseconds)                             */
/* ------------------------------------------------------------------ */

/* Mirrors serial_4way.c. The start-of-frame byte is waited for
 * indefinitely, as BLHeliSuite32 expects. */
#define ESC4W_CMD_TIMEOUT_US         50000
#define ESC4W_ARG_TIMEOUT_US         25000
#define ESC4W_DAT_TIMEOUT_US         10000
#define ESC4W_CRC_TIMEOUT_US         10000

/* ------------------------------------------------------------------ */
/* 5. 4-way interface identity                                         */
/* ------------------------------------------------------------------ */

#define ESC4W_INTERFACE_NAME_STR     "m4wFCIntf"
#define ESC4W_VER_MAIN               20
#define ESC4W_VER_SUB_1              0
#define ESC4W_VER_SUB_2              6
#define ESC4W_PROTOCOL_VER           108

/* ------------------------------------------------------------------ */
/* 6. MSP identity                                                     */
/* ------------------------------------------------------------------ */

/* The configurator gates features on these. Claiming a recent
 * Betaflight API keeps ESC Configurator and BLHeliSuite32 happy. */
#define ESC4W_MSP_PROTOCOL_VERSION   0
#define ESC4W_MSP_API_MAJOR          1
#define ESC4W_MSP_API_MINOR          46

#define ESC4W_MSP_FC_VARIANT         "BTFL"      /* exactly 4 chars */
#define ESC4W_MSP_FC_VER_MAJOR       4
#define ESC4W_MSP_FC_VER_MINOR       5
#define ESC4W_MSP_FC_VER_PATCH       1

#define ESC4W_MSP_BOARD_ID           "ESPC"      /* exactly 4 chars */
#if ESC4W_USB_TINYUSB
#  define ESC4W_MSP_TARGET_NAME      "ESP32S3_4WAY"
#  define ESC4W_MSP_BOARD_NAME       "esc4way-s3"
#else
#  define ESC4W_MSP_TARGET_NAME      "ESP32C3_4WAY"
#  define ESC4W_MSP_BOARD_NAME       "esc4way-c3"
#endif
#define ESC4W_MSP_MANUFACTURER_ID    "CUST"      /* exactly 4 chars */

#define ESC4W_MSP_BUILD_DATE         "Jan 01 2026"   /* exactly 11 */
#define ESC4W_MSP_BUILD_TIME         "00:00:00"      /* exactly 8  */
#define ESC4W_MSP_GIT_REVISION       "esc4way"       /* exactly 7  */

/* 12-byte unique ID reported by MSP_UID. Derived from the chip MAC at
 * runtime when set to 1, otherwise the fixed value below is used. */
#define ESC4W_MSP_UID_FROM_MAC       1
#define ESC4W_MSP_UID_FALLBACK       { 0xE5, 0xC4, 0x00, 0x00, 0xC3, 0x32, \
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

/* Motor count advertised by MSP_MOTOR / MSP_MOTOR_CONFIG. Some
 * configurator versions cross-check this against the ESC count. */
#define ESC4W_MSP_MOTOR_COUNT        ESC4W_ESC_COUNT
#define ESC4W_MSP_MOTOR_POLES        14

/* ------------------------------------------------------------------ */
/* 6b. USB device identity (TinyUSB / ESP32-S3 only)                   */
/* ------------------------------------------------------------------ */

/* The AM32 Configurator only offers ports whose USB vendor ID is on its
 * hardcoded allow-list; 0x303A (Espressif) is not on it, so a stock
 * ESP32 never appears in the Chrome port picker. Presenting the STM32
 * Virtual COM Port identity puts the programmer on that list, which is
 * what every Betaflight-derived tool expects from a flight controller.
 *
 * 0x0483:0x5740 is STMicroelectronics' VCP. It is not yours; it is fine
 * for a bench tool, but do not ship products with it. */
#define ESC4W_USB_VID                0x0483
#define ESC4W_USB_PID                0x5740
#define ESC4W_USB_MANUFACTURER       "STMicroelectronics"
#define ESC4W_USB_PRODUCT            "STM32 Virtual ComPort"
#define ESC4W_USB_SERIAL             "esc4way-s3"
#define ESC4W_USB_CDC_NAME           "esc4way CDC"

/* ------------------------------------------------------------------ */
/* 7. Buffers and diagnostics                                          */
/* ------------------------------------------------------------------ */

/* 4-way payloads are capped at 256 bytes by the protocol. */
#define ESC4W_PARAM_BUF_SIZE         256

/* Reply assembly buffer: 5 header + 256 payload + ACK + 2 CRC. */
#define ESC4W_REPLY_BUF_SIZE         (ESC4W_PARAM_BUF_SIZE + 16)

/* UART driver RX ring for the one-wire link. */
#define ESC4W_UART_RX_BUF_SIZE       512

/* Diagnostic trace on a second USB CDC port (S3/TinyUSB only) — never
 * on the protocol port, where any stray byte corrupts the MSP/4-way
 * stream. Requires CONFIG_TINYUSB_CDC_COUNT=2 (set in sdkconfig.defaults).
 * On Linux: protocol on /dev/ttyACM0, trace on /dev/ttyACM1, e.g.
 *     picocom /dev/ttyACM1
 * Silent unless a terminal holds the trace port open. */
#define ESC4W_LOG_ENABLE             1
