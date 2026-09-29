#include "driver/gpio.h"
#include "esp_timer.h"

#include "config.h"
#include "led.h"

#if ESC4W_LED_PIN >= 0

static esp_timer_handle_t s_settle;
static volatile bool      s_on;

static void led_write(bool on)
{
    s_on = on;
    gpio_set_level((gpio_num_t)ESC4W_LED_PIN, ESC4W_LED_ACTIVE_LOW ? !on : on);
}

/* Runs in the esp_timer task. A single GPIO write, so racing with the
 * 4-way loop at worst costs one flash phase. */
static void led_settle_cb(void *arg)
{
    (void)arg;
    led_write(true);
}

void led_init(void)
{
    gpio_reset_pin((gpio_num_t)ESC4W_LED_PIN);
    gpio_set_direction((gpio_num_t)ESC4W_LED_PIN, GPIO_MODE_OUTPUT);
    led_write(false);

    const esp_timer_create_args_t args = {
        .callback = led_settle_cb,
        .name     = "led_settle",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_settle));
}

void led_set(bool on)
{
    /* Stop first, or a pending settle would turn the LED back on after
     * the session has ended. Fails harmlessly when not running. */
    esp_timer_stop(s_settle);
    led_write(on);
}

void led_activity(void)
{
    esp_timer_stop(s_settle);
    led_write(!s_on);
    esp_timer_start_once(s_settle, ESC4W_LED_ACTIVITY_HOLD_MS * 1000ULL);
}

#else

void led_init(void) { }
void led_set(bool on) { (void)on; }
void led_activity(void) { }

#endif
