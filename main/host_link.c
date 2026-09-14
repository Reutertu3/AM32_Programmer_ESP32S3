/*
 * host_link.c — byte transport to the configurator.
 *
 * Two backends behind one API, selected by ESC4W_USB_TINYUSB in config.h:
 *
 *   ESP32-S3 : TinyUSB over USB-OTG. The only backend that can set a
 *              custom VID/PID, which is what makes the device visible to
 *              tools that filter the port list by vendor — the AM32
 *              Configurator does.
 *   ESP32-C3 : native USB Serial/JTAG. Descriptors live in ROM; always
 *              enumerates as 303a:1001.
 *
 * Never enable both: each installs its own ISR on the USB peripheral.
 */
#include "config.h"
#include "host_link.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#if ESC4W_USB_TINYUSB
#  include "tinyusb.h"
#  include "tusb_cdc_acm.h"
#else
#  include "driver/usb_serial_jtag.h"
#endif

/* ------------------------------------------------------------------ */
#if ESC4W_USB_TINYUSB
/* ------------------------------------------------------------------ */

#define CDC_PORT TINYUSB_CDC_ACM_0

void host_link_init(void)
{
    static const tusb_desc_device_t device_desc = {
        .bLength            = sizeof(tusb_desc_device_t),
        .bDescriptorType    = TUSB_DESC_DEVICE,
        .bcdUSB             = 0x0200,
        /* IAD: CDC-ACM is a two-interface function. */
        .bDeviceClass       = TUSB_CLASS_MISC,
        .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
        .bDeviceProtocol    = MISC_PROTOCOL_IAD,
        .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor           = ESC4W_USB_VID,
        .idProduct          = ESC4W_USB_PID,
        .bcdDevice          = 0x0200,
        .iManufacturer      = 0x01,
        .iProduct           = 0x02,
        .iSerialNumber      = 0x03,
        .bNumConfigurations = 0x01,
    };

    /* A compound literal inside a function has automatic storage
     * duration, so it cannot initialise a static array in C. Give the
     * LANGID its own object. */
    static const char langid[] = { 0x09, 0x04, 0x00 };

    static const char *string_desc[] = {
        langid,                           /* index 0: LANGID en-US */
        ESC4W_USB_MANUFACTURER,
        ESC4W_USB_PRODUCT,
        ESC4W_USB_SERIAL,
        ESC4W_USB_CDC_NAME,
    };

    tinyusb_config_t tusb_cfg = { 0 };
    tusb_cfg.device_descriptor       = &device_desc;
    tusb_cfg.string_descriptor       = string_desc;
    tusb_cfg.string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]);
    tusb_cfg.external_phy            = false;
    /* NULL lets esp_tinyusb build the standard single-CDC configuration
     * descriptor. Keeps this source portable across component versions
     * that added fields to this struct. */
    tusb_cfg.configuration_descriptor = NULL;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = { 0 };
    acm_cfg.usb_dev          = TINYUSB_USBDEV_0;
    acm_cfg.cdc_port         = CDC_PORT;
    /* No rx_unread_buf_sz here: it is deprecated and ignored in
     * esp_tinyusb >= 1.7. The RX ring is CONFIG_TINYUSB_CDC_RX_BUFSIZE. */
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
}

bool host_read_byte(uint8_t *b, uint32_t timeout_us)
{
    size_t rx = 0;
    const bool forever = (timeout_us == 0);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_us;

    for (;;) {
        if (tinyusb_cdcacm_read(CDC_PORT, b, 1, &rx) == ESP_OK && rx == 1) {
            return true;
        }
        if (!forever && esp_timer_get_time() >= deadline) {
            return false;
        }
        /* tinyusb_cdcacm_read() is non-blocking — it reports zero bytes
         * and returns. Without an explicit yield this spins at 100% on
         * the core app_main is pinned to, starves the idle task and
         * trips the task watchdog. Costs at most one tick of latency,
         * and only when the RX ring is already empty. */
        vTaskDelay(1);
    }
}

void host_write(const uint8_t *buf, uint16_t len)
{
    uint16_t sent = 0;
    while (sent < len) {
        size_t n = tinyusb_cdcacm_write_queue(CDC_PORT, buf + sent, len - sent);
        if (n == 0) {
            /* Queue full: push what is already there, then retry once. */
            tinyusb_cdcacm_write_flush(CDC_PORT, pdMS_TO_TICKS(100));
            n = tinyusb_cdcacm_write_queue(CDC_PORT, buf + sent, len - sent);
            if (n == 0) {
                break;              /* host gone */
            }
        }
        sent += (uint16_t)n;
    }
}

void host_flush(void)
{
    tinyusb_cdcacm_write_flush(CDC_PORT, pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------------ */
#else   /* ESP32-C3 native USB Serial/JTAG */
/* ------------------------------------------------------------------ */

void host_link_init(void)
{
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

bool host_read_byte(uint8_t *b, uint32_t timeout_us)
{
    if (timeout_us == 0) {
        for (;;) {
            if (usb_serial_jtag_read_bytes(b, 1, pdMS_TO_TICKS(100)) == 1) {
                return true;
            }
        }
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_us;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            return false;
        }
        uint32_t wait_ms = (uint32_t)((deadline - now) / 1000) + 1;
        if (usb_serial_jtag_read_bytes(b, 1, pdMS_TO_TICKS(wait_ms)) == 1) {
            return true;
        }
    }
}

void host_write(const uint8_t *buf, uint16_t len)
{
    uint16_t sent = 0;
    while (sent < len) {
        int n = usb_serial_jtag_write_bytes(buf + sent, len - sent,
                                            pdMS_TO_TICKS(100));
        if (n <= 0) { break; }
        sent += (uint16_t)n;
    }
}

void host_flush(void)
{
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
}

#endif
