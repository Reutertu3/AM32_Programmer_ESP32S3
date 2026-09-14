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

/* How the two pad APIs interact (checked against IDF 6.0.1) — every
 * ordering below depends on it:
 *
 *   esp_rom_gpio_connect_out_signal(pin, sig)
 *       routes sig to the pad AND sets the pad's output-enable bit.
 *       With SIG_GPIO_OUT_IDX the pad is then driven from the GPIO
 *       output register — it does not release the pad.
 *   gpio_set_direction(pin, ...OUTPUT...)
 *       resets the routing to SIG_GPIO_OUT_IDX before enabling the
 *       driver, silently disconnecting any peripheral signal.
 *   gpio_set_direction(pin, GPIO_MODE_INPUT)
 *       clears output-enable and takes it away from the peripheral;
 *       the pad goes high-Z whatever is routed to it.
 *
 * So: routing a peripheral output must come LAST, and releasing a pad
 * must END with gpio_set_direction(INPUT). */

/* Park a pin: detached from the UART, pulled-up input, line idle high.
 * The level preload matters: the detach briefly drives the pad from the
 * GPIO output register, which reads 0 after gpio_reset_pin(). Loaded
 * with 1 it matches the idle line, so there is no glitch. */
static void esc_pin_park(int pin)
{
    gpio_set_level(pin, 1);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
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

        /* uart_read_bytes() only sees what the ISR has moved into the
         * ring buffer, and by default the ISR runs when 120 bytes are
         * waiting in the FIFO or after 10 idle character times — 5.2 ms
         * at 19200 baud. Every bootloader reply is shorter than 120
         * bytes, and 5.2 ms is longer than the 3 ms byte timeout, so
         * the echo and the reply would always arrive too late. Deliver
         * each byte as it lands; at 19200 baud that is at most ~1900
         * interrupts per second. */
        ESP_ERROR_CHECK(uart_set_rx_full_threshold(ESC_UART, 1));
        ESP_ERROR_CHECK(uart_set_rx_timeout(ESC_UART, 1));
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

uint16_t esc_io_write(const uint8_t *buf, uint16_t len)
{
    if (s_selected < 0 || len == 0) {
        return 0;
    }
    const int pin = s_esc_pins[s_selected];

    uart_flush_input(ESC_UART);

    /* INPUT_OUTPUT keeps the pad input enabled while we drive it, so
     * the UART receives a clean, countable echo of our own frame.
     * The direction call must come first: it resets the routing to
     * plain GPIO, so called after the UART hook-up it would disconnect
     * TX and drive the register level for the whole frame instead —
     * nothing would ever reach the ESC. With the park preload the pad
     * sits at 1 until TX (idle high) takes over, so no edge either. */
#if ESC4W_TX_PUSH_PULL
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
#else
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
#endif
    esp_rom_gpio_connect_out_signal(pin, esc_tx_signal(), false, false);

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
    return (uint16_t)(len - remaining);
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

    /* Plain GPIO output: gpio_set_direction(OUTPUT) does the routing
     * itself. Load the level first so the pulse starts at the moment
     * the driver is enabled. */
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(pin, 1);

    esc_pin_park(pin);
    esp_rom_gpio_connect_in_signal(pin, esc_rx_signal(), false);
    esc_io_flush();
}

int esc_io_pin(uint8_t index)
{
    return index < ESC4W_ESC_COUNT ? s_esc_pins[index] : -1;
}

int esc_io_level(uint8_t index)
{
    return index < ESC4W_ESC_COUNT ? gpio_get_level(s_esc_pins[index]) : -1;
}
