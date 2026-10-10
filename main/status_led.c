#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "sofa-led";

/* Half of the blink period: 500 ms off + 500 ms on = 1 Hz. */
#define BLINK_HALF_PERIOD_US (500 * 1000)

/*
 * A Kconfig bool set to "n" is *undefined* in sdkconfig.h, not defined as 0. It
 * is safe inside #if but must never be used as a C expression, because
 *   CONFIG_SOFA_STATUS_LED_ACTIVE_LOW ? "active low" : "active high"
 * stops compiling the moment the option is turned off. Resolve it once, here.
 */
#if CONFIG_SOFA_STATUS_LED_ACTIVE_LOW
#define LED_ON_LEVEL 0
#define LED_POLARITY "active low"
#else
#define LED_ON_LEVEL 1
#define LED_POLARITY "active high"
#endif

static esp_timer_handle_t s_blink_timer;
static bool s_on;

/*
 * Two tasks call status_led_set_ready(): the BLE state callback on the NimBLE
 * host task, and the WiFi event callback on the event task. Without the latch
 * below they could both reach esp_timer_start_periodic() and the loser would
 * trip ESP_ERROR_CHECK.
 */
static SemaphoreHandle_t s_lock;
static bool s_ready;

static void led_write(bool on)
{
    gpio_set_level(CONFIG_SOFA_STATUS_LED_GPIO, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

static void blink_cb(void *arg)
{
    (void)arg;
    s_on = !s_on;
    led_write(s_on);
}

esp_err_t status_led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(gpio_reset_pin(CONFIG_SOFA_STATUS_LED_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(CONFIG_SOFA_STATUS_LED_GPIO, GPIO_MODE_OUTPUT));

    const esp_timer_create_args_t args = {
        .callback = &blink_cb,
        .name = "sofa-led",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_blink_timer));

    ESP_LOGI(TAG, "status LED on GPIO%d, %s; blinking until the sofa is ready",
             CONFIG_SOFA_STATUS_LED_GPIO, LED_POLARITY);

    status_led_set_ready(false);
    return ESP_OK;
}

void status_led_set_ready(bool ready)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ready == s_ready) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_ready = ready;

    /* stop() reports ESP_ERR_INVALID_STATE when the timer is not running, and
     * both branches below mean "make sure it is stopped", so ignore that. */
    (void)esp_timer_stop(s_blink_timer);

    if (ready) {
        s_on = true;
        led_write(true);
        ESP_LOGI(TAG, "everything up -> LED steady on");
    } else {
        s_on = false;
        led_write(false);
        ESP_LOGI(TAG, "not ready -> LED blinking");
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_blink_timer, BLINK_HALF_PERIOD_US));
    }
    xSemaphoreGive(s_lock);
}
