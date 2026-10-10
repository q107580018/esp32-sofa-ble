#include "sofa_control.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "sofa_ble.h"

static const char *TAG = "sofa-ctl";

static esp_timer_handle_t s_jog_timer;
static esp_timer_handle_t s_hold_timer;

/* 0 = no watchdog. Written once by the HTTP layer at start-up. */
static uint32_t s_hold_timeout_ms;

/* Set from the NimBLE host task (status frame) and from the console, so keep
 * the type small enough that a torn read is impossible. */
static volatile bool s_one_shot = true;
static volatile bool s_forward = true;

static bool s_held;
static sofa_button_t s_held_btn;
static sofa_button_t s_last_btn;

static void on_status(const sofa_status_t *status)
{
    if (status->has_mode) {
        s_one_shot = (status->mode == 1);
    }
    if (status->has_dir) {
        s_forward = (status->dir == 1);
    }
    if (status->has_motor_stop && s_held) {
        /* The sofa says the motor finished on its own (end stop, or a one-shot
         * run completed) - stop repeating. */
        ESP_LOGI(TAG, "%s finished on the sofa side; stopping the repeat",
                 sofa_motor_name(status->motor));
        s_held = false;
        (void)esp_timer_stop(s_jog_timer);
        (void)esp_timer_stop(s_hold_timer);
    }
}

static void on_link_state(sofa_state_t state)
{
    if (state != SOFA_STATE_READY && s_held) {
        /* Link went away mid-press: nothing left to release. */
        ESP_LOGW(TAG, "link lost while %s was held", sofa_button_name(s_held_btn));
        s_held = false;
        (void)esp_timer_stop(s_jog_timer);
        (void)esp_timer_stop(s_hold_timer);
    }
}

void sofa_control_on_link_state(sofa_state_t state)
{
    on_link_state(state);
}

static void stop_repeat(bool release);

/* (Re)arms the hold watchdog. No-op when it is disabled. */
static void arm_hold_timer(void)
{
    if (s_hold_timeout_ms == 0 || !s_held) {
        return;
    }
    (void)esp_timer_stop(s_hold_timer);
    uint64_t us = (uint64_t)s_hold_timeout_ms * 1000ull;
    if (us < 1000) {
        us = 1000;
    }
    (void)esp_timer_start_once(s_hold_timer, us);
}

static void hold_timer_cb(void *arg)
{
    (void)arg;
    /* Loud on purpose: this means something asked the sofa to move and then
     * stopped talking, and the sofa is about to stop on its own. */
    ESP_LOGW(TAG, "no keep-alive for %u ms - releasing %s",
             (unsigned)s_hold_timeout_ms, sofa_button_name(s_held_btn));
    stop_repeat(true);
}

static void jog_timer_cb(void *arg)
{
    (void)arg;

    uint8_t frame[SOFA_FRAME_LEN];
    size_t len = sofa_frame_press(s_held_btn, true, frame);
    esp_err_t err = sofa_ble_send(frame, len);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        /* ESP_ERR_TIMEOUT just means the queue was momentarily full, which the
         * next tick absorbs. Anything else means the link is gone. */
        ESP_LOGW(TAG, "jog repeat failed: %s", esp_err_to_name(err));
        s_held = false;
        (void)esp_timer_stop(s_jog_timer);
        (void)esp_timer_stop(s_hold_timer);
    }
}

void sofa_control_init(void)
{
    const esp_timer_create_args_t jog_args = {
        .callback = &jog_timer_cb,
        .name = "sofa-jog",
    };
    ESP_ERROR_CHECK(esp_timer_create(&jog_args, &s_jog_timer));

    const esp_timer_create_args_t hold_args = {
        .callback = &hold_timer_cb,
        .name = "sofa-hold",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hold_args, &s_hold_timer));

    sofa_ble_set_status_cb(on_status);

    ESP_LOGI(TAG, "jog repeat every %d ms", CONFIG_SOFA_JOG_REPEAT_MS);
}

void sofa_control_hold_watchdog(uint32_t timeout_ms)
{
    s_hold_timeout_ms = timeout_ms;
    if (timeout_ms == 0) {
        (void)esp_timer_stop(s_hold_timer);
        return;
    }
    arm_hold_timer();
}

void sofa_control_keep_alive(void)
{
    arm_hold_timer();
}

bool sofa_control_one_shot(void)
{
    return s_one_shot;
}

bool sofa_control_forward(void)
{
    return s_forward;
}

bool sofa_control_is_held(void)
{
    return s_held;
}

sofa_button_t sofa_control_held_button(void)
{
    return s_held ? s_held_btn : 0;
}

static void stop_repeat(bool release)
{
    if (!s_held) {
        return;
    }

    (void)esp_timer_stop(s_jog_timer);
    (void)esp_timer_stop(s_hold_timer);
    /* Drop repeats already queued but not yet on the wire, or the sofa keeps
     * moving after the release frame. */
    sofa_ble_flush_tx();

    sofa_button_t btn = s_held_btn;
    s_held = false;

    if (release) {
        uint8_t frame[SOFA_FRAME_LEN];
        size_t len = sofa_frame_release(btn, frame);
        if (sofa_ble_send(frame, len) == ESP_OK) {
            ESP_LOGI(TAG, "released %s", sofa_button_name(btn));
        }
    }
}

esp_err_t sofa_control_move(sofa_button_t btn)
{
    if (!sofa_ble_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* One button at a time, exactly like the app's touchscreen. */
    stop_repeat(true);

    uint8_t frame[SOFA_FRAME_LEN];
    bool jog = !s_one_shot;
    size_t len = sofa_frame_press(btn, jog, frame);

    esp_err_t err = sofa_ble_send(frame, len);
    if (err != ESP_OK) {
        return err;
    }
    s_last_btn = btn;

    if (jog) {
        s_held = true;
        s_held_btn = btn;
        uint64_t period = (uint64_t)CONFIG_SOFA_JOG_REPEAT_MS * 1000ull;
        if (period < 1000) {
            period = 1000;
        }
        ESP_ERROR_CHECK(esp_timer_start_periodic(s_jog_timer, period));
        arm_hold_timer();
        ESP_LOGI(TAG, "jogging %s (repeat every %d ms)",
                 sofa_button_name(btn), CONFIG_SOFA_JOG_REPEAT_MS);
    } else {
        ESP_LOGI(TAG, "pressed %s (one-shot)", sofa_button_name(btn));
    }
    return ESP_OK;
}

esp_err_t sofa_control_set_one_shot(bool one_shot)
{
    uint8_t frame[SOFA_FRAME_LEN];
    size_t len = sofa_frame_mode(one_shot, frame);

    esp_err_t err = sofa_ble_send(frame, len);
    if (err == ESP_OK) {
        s_one_shot = one_shot;
        ESP_LOGI(TAG, "movement mode -> %s", one_shot ? "one-shot" : "jog");
    }
    return err;
}

esp_err_t sofa_control_set_forward(bool forward)
{
    uint8_t frame[SOFA_FRAME_LEN];
    size_t len = sofa_frame_direction(forward, frame);
    esp_err_t err = sofa_ble_send(frame, len);
    if (err == ESP_OK) {
        s_forward = forward;
    }
    return err;
}

esp_err_t sofa_control_stop_button(sofa_button_t btn)
{
    if (!sofa_ble_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_held && s_held_btn == btn) {
        stop_repeat(true);
        return ESP_OK;
    }

    uint8_t frame[SOFA_FRAME_LEN];
    size_t len;
    /*
     * Nothing is being held. In one-shot mode the app stops a run by pressing
     * the same button again; in jog mode the release frame is what ends it.
     */
    if (s_one_shot) {
        len = sofa_frame_press(btn, false, frame);
    } else {
        len = sofa_frame_release(btn, frame);
    }
    s_last_btn = btn;
    return sofa_ble_send(frame, len);
}

esp_err_t sofa_control_stop_target(sofa_button_t up, sofa_button_t down)
{
    if (!sofa_ble_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_held && (s_held_btn == up || s_held_btn == down)) {
        stop_repeat(true);
        return ESP_OK;
    }

    if (s_one_shot) {
        /* See the header: pressing a guess would start the sofa moving. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Jog mode, nothing held: releasing a stationary motor is a no-op. */
    esp_err_t err = sofa_control_stop_button(up);
    if (err == ESP_OK) {
        err = sofa_control_stop_button(down);
    }
    return err;
}

esp_err_t sofa_control_stop(void)
{
    if (s_held) {
        stop_repeat(true);
        return ESP_OK;
    }
    if (s_last_btn == 0) {
        /* Never pressed anything. Worth saying precisely, because it is the one
         * failure here that is not the link's fault. */
        return ESP_ERR_NOT_FOUND;
    }
    return sofa_control_stop_button(s_last_btn);
}

void sofa_control_abort(void)
{
    (void)esp_timer_stop(s_jog_timer);
    (void)esp_timer_stop(s_hold_timer);
    s_held = false;
    sofa_ble_flush_tx();
}

esp_err_t sofa_control_send_raw(const uint8_t *data, size_t len)
{
    return sofa_ble_send(data, len);
}
