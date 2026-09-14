/*
 * ESP32-S3 / ESP32-C3 USB programmer for BLHeli_S / BLHeli_32 / AM32
 * 4-in-1 ESCs.
 *
 * Presents itself over native USB CDC as a flight controller: answers
 * enough MSP to satisfy a configurator, then switches the same endpoint
 * into the BLHeli 4-way protocol. The ESC index carried by
 * cmd_DeviceInitFlash selects which of the four signal pins the
 * one-wire transaction runs on, so a single USB connection addresses
 * all four ESCs of a 4-in-1.
 *
 * ESP-IDF project. Everything board-specific lives in config.h.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "config.h"
#include "host_link.h"
#include "msp.h"
#include "serial_4way.h"
#include "esc_io.h"

#if ESC4W_LED_PIN >= 0
static void led_init(void)
{
    gpio_reset_pin((gpio_num_t)ESC4W_LED_PIN);
    gpio_set_direction((gpio_num_t)ESC4W_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)ESC4W_LED_PIN, ESC4W_LED_ACTIVE_LOW ? 1 : 0);
}

static void led_set(bool on)
{
    gpio_set_level((gpio_num_t)ESC4W_LED_PIN, ESC4W_LED_ACTIVE_LOW ? !on : on);
}
#else
static void led_init(void) { }
static void led_set(bool on) { (void)on; }
#endif

static void esc4way_setup(void)
{
    led_init();
    /* Park the ESC pins as pulled-up inputs from boot. The one-wire idle
     * state is high, and a floating signal line into a powered ESC is
     * worth avoiding. esc4way_init() re-runs this when passthrough
     * starts; calling it twice is harmless. */
    esc_io_init();
    host_link_init();
    msp_init();
}

/* Printed whenever a terminal opens the trace port, so there is never
 * any doubt which of the two ports you are looking at. */
static void trace_banner(void)
{
    host_log("%s", "");
    host_log("=== esc4way trace port (USB interface 2) ===");
    host_log("The configurator belongs on the OTHER port (interface 0).");
    esc4way_log_levels("config:");
    host_log("Waiting for MSP...");
}

/* One pass of the service loop. Returns after at most 100 ms idle. */
static void esc4way_service(void)
{
    uint8_t b;

    if (host_log_just_opened()) {
        trace_banner();
    }
    if (host_read_byte(&b, 100000)) {
        msp_process_byte(b);

        if (msp_take_passthrough_request()) {
            led_set(true);
            /* Blocks until the host sends cmd_InterfaceExit. */
            esc4way_process();
            led_set(false);
            msp_init();
        }
    }
}

void app_main(void)
{
    esc4way_setup();
    for (;;) {
        esc4way_service();
    }
}
