#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
/* soc/uart_periph.h (and its uart_periph_signal[] table) was removed in
 * ESP-IDF 6.0; the per-target signal indices below replace it. */
#include "soc/gpio_sig_map.h"

#include "config.h"
#include "esc_io.h"

static const int s_esc_pins[ESC4W_ESC_COUNT] = ESC4W_ESC_PINS;

static int     s_selected = -1;
static bool    s_inited   = false;
static uint8_t s_scratch[64];

#define ESC_UART ((uart_port_t)ESC4W_UART_NUM)
#if ESC4W_UART_NUM == 1
#  define ESC_TX_SIG  U1TXD_OUT_IDX
#  define ESC_RX_SIG  U1RXD_IN_IDX
#elif ESC4W_UART_NUM == 2
#  define ESC_TX_SIG  U2TXD_OUT_IDX
#  define ESC_RX_SIG  U2RXD_IN_IDX
#else
#  error "pick UART1 or UART2 — UART0 is the console"
#endif

static inline uint32_t esc_tx_signal(void) { return ESC_TX_SIG; }
static inline uint32_t esc_rx_signal(void) { return ESC_RX_SIG; }

/* Park a pin: detached from the UART, pulled-up input, line idle high.
 *
 * Order matters. Detaching the UART signal first re-points a pad whose
 * output driver is still enabled (INPUT_OUTPUT, during a transmit) at
 * the GPIO output register, which holds 0 after gpio_reset_pin() — that
 * yanks the idle-high line low for the microseconds until the direction
 * change lands, right before we start listening for the reply.
 * gpio_set_direction(INPUT) clears the enable bit and takes OE away from
 * the peripheral, so dropping the driver first is enough; the level
 * write makes even a re-enable benign. */
static void esc_pin_park(int pin)
{
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
#if ESC4W_RX_INTERNAL_PULLUP
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
#else
    gpio_set_pull_mode(pin, GPIO_FLOATING);
#endif
}

uint8_t esc_io_init(void)
{
    if (!s_inited) {
        const uart_config_t cfg = {
            .baud_rate  = ESC4W_BL_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(ESC_UART, ESC4W_UART_RX_BUF_SIZE,
                                            0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(ESC_UART, &cfg));
        s_inited = true;
    }

    for (int i = 0; i < ESC4W_ESC_COUNT; i++) {
        gpio_reset_pin(s_esc_pins[i]);
        esc_pin_park(s_esc_pins[i]);
    }
    s_selected = -1;

    return ESC4W_ESC_COUNT;
}

void esc_io_release(void)
{
    for (int i = 0; i < ESC4W_ESC_COUNT; i++) {
        esc_pin_park(s_esc_pins[i]);
    }
    s_selected = -1;
}

bool esc_io_select(uint8_t index)
{
    if (index >= ESC4W_ESC_COUNT) {
        return false;
    }
    if (s_selected == (int)index) {
        esc_io_flush();
        return true;
    }
    if (s_selected >= 0) {
        esc_pin_park(s_esc_pins[s_selected]);
    }

    const int pin = s_esc_pins[index];
    esc_pin_park(pin);
    /* RX stays matrixed to this pad for the whole session; TX is only
     * attached while transmitting so the pad never fights the ESC. */
    esp_rom_gpio_connect_in_signal(pin, esc_rx_signal(), false);

    s_selected = index;
    esc_io_flush();
    return true;
}

void esc_io_flush(void)
{
    uart_flush_input(ESC_UART);
}

void esc_io_write(const uint8_t *buf, uint16_t len)
{
    if (s_selected < 0 || len == 0) {
        return;
    }
    const int pin = s_esc_pins[s_selected];

    uart_flush_input(ESC_UART);

    /* INPUT_OUTPUT keeps the pad input enabled while we drive it, so
     * the UART receives a clean, countable echo of our own frame
     * instead of a floating line and spurious framing errors. */
    esp_rom_gpio_connect_out_signal(pin, esc_tx_signal(), false, false);
#if ESC4W_TX_PUSH_PULL
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
#else
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
#endif

    uart_write_bytes(ESC_UART, (const char *)buf, len);
    uart_wait_tx_done(ESC_UART, pdMS_TO_TICKS(1000));

    /* Release the line before the ESC starts answering. */
    esc_pin_park(pin);
    esp_rom_gpio_connect_in_signal(pin, esc_rx_signal(), false);

    /* Drain the echo. Read it rather than blind-flush so a reply that
     * arrives immediately after the echo is not thrown away. */
    uint16_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk = remaining > sizeof(s_scratch) ? sizeof(s_scratch)
                                                       : remaining;
        uint16_t got = esc_io_read(s_scratch, chunk, ESC4W_BYTE_TIMEOUT_MS);
        if (got == 0) {
            break;  /* echo lost (open-drain, no pull-up) — carry on */
        }
        remaining -= got;
    }
}

uint16_t esc_io_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    if (s_selected < 0 || len == 0) {
        return 0;
    }

    uint16_t got = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (got < len) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            break;
        }
        uint32_t wait_ms = (uint32_t)((deadline - now) / 1000) + 1;
        int n = uart_read_bytes(ESC_UART, buf + got, len - got,
                                pdMS_TO_TICKS(wait_ms));
        if (n > 0) {
            got += (uint16_t)n;
            /* Per-byte timeout: restart the window after progress. */
            deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        }
    }
    return got;
}

void esc_io_pulse_low(uint32_t ms)
{
    if (s_selected < 0) {
        return;
    }
    const int pin = s_esc_pins[s_selected];

    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    /* Load the level before enabling the driver, so the pulse starts at
     * its leading edge instead of a brief high from the parked state. */
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, 1);

    esc_pin_park(pin);
    esp_rom_gpio_connect_in_signal(pin, esc_rx_signal(), false);
    esc_io_flush();
}
