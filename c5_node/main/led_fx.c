/*
 * led_fx.c — POSEIDON status LED on the C5's onboard WS2812.
 *
 * Pin assumption: GPIO27 (Espressif ESP32-C5-DevKitC-1). If your
 * variant uses a different pin, change LED_GPIO below.
 *
 * Visual language:
 *   IDLE      slow magenta breathe (we're alive, awaiting commands)
 *   SCAN      cyan pulse (radio is sweeping)
 *   ATTACK    magenta+cyan alternating fast (deauth in progress)
 *   ZB_RX     brief white blip per 802.15.4 frame
 *   PING      cyan flash for 200ms
 *
 * Driver: ESP-IDF's RMT-based bit-banging of WS2812 timing. One LED.
 */
#include "led_fx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include <math.h>
#include <string.h>

#define LED_GPIO            27       /* Change if your C5 variant uses a different pin */
#define WS2812_RESOLUTION_HZ 10000000

static const char *TAG = "led_fx";

static rmt_channel_handle_t s_chan      = NULL;
static rmt_encoder_handle_t s_encoder   = NULL;
static rmt_transmit_config_t s_tx_cfg   = { .loop_count = 0 };
static volatile led_mode_t   s_mode     = LED_MODE_OFF;
static TaskHandle_t          s_task     = NULL;

/* WS2812 bit timing in ns: T0H=350, T0L=900, T1H=900, T1L=350. */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder,
                            rmt_channel_handle_t channel,
                            const void *primary_data,
                            size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    switch (enc->state) {
    case 0: /* data */
        encoded += enc->bytes_encoder->encode(enc->bytes_encoder, channel,
                                              primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL; goto out;
        }
        /* fall through */
    case 1: /* reset */
        encoded += enc->copy_encoder->encode(enc->copy_encoder, channel,
                                             &enc->reset_code, sizeof(enc->reset_code),
                                             &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            enc->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) state |= RMT_ENCODING_MEM_FULL;
    }
out:
    *ret_state = state;
    return encoded;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = 0;
    return ESP_OK;
}

static esp_err_t make_ws2812_encoder(rmt_encoder_handle_t *out)
{
    ws2812_encoder_t *enc = calloc(1, sizeof(*enc));
    enc->base.encode  = ws2812_encode;
    enc->base.del     = ws2812_encoder_del;
    enc->base.reset   = ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bcfg = {
        .bit0 = { .level0=1, .duration0=4, .level1=0, .duration1=9 },  /* ~T0H/T0L */
        .bit1 = { .level0=1, .duration0=9, .level1=0, .duration1=4 },  /* ~T1H/T1L */
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&bcfg, &enc->bytes_encoder);

    rmt_copy_encoder_config_t ccfg = {};
    rmt_new_copy_encoder(&ccfg, &enc->copy_encoder);

    enc->reset_code = (rmt_symbol_word_t){ .level0=0, .duration0=250,
                                            .level1=0, .duration1=250 };
    *out = &enc->base;
    return ESP_OK;
}

static void push_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };  /* WS2812 order */
    rmt_transmit(s_chan, s_encoder, grb, 3, &s_tx_cfg);
    rmt_tx_wait_all_done(s_chan, 50);
}

/* Pre-multiplied helpers for clean fade math. */
static inline uint8_t scale8(uint8_t v, uint8_t s) { return (uint16_t)v * s / 255; }

static void fx_task(void *_)
{
    uint32_t t = 0;
    uint32_t last_blip = 0;
    while (1) {
        led_mode_t mode = s_mode;
        switch (mode) {
        case LED_MODE_OFF:
            push_rgb(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
            break;

        case LED_MODE_IDLE: {
            /* Slow magenta breathe (1.5s period). */
            float ph = (t % 1500) / 1500.0f;
            uint8_t lvl = (uint8_t)(8 + 30 * (0.5f + 0.5f * sinf(ph * 6.2832f)));
            push_rgb(lvl, 0, lvl);
            vTaskDelay(pdMS_TO_TICKS(40));
            t += 40;
            break;
        }

        case LED_MODE_SCAN: {
            /* Cyan pulse (faster, 600ms period). */
            float ph = (t % 600) / 600.0f;
            uint8_t lvl = (uint8_t)(20 + 80 * (0.5f + 0.5f * sinf(ph * 6.2832f)));
            push_rgb(0, lvl, lvl);
            vTaskDelay(pdMS_TO_TICKS(30));
            t += 30;
            break;
        }

        case LED_MODE_ATTACK: {
            /* Magenta/cyan strobe alternating every 80ms. */
            if ((t / 80) & 1) push_rgb(120, 0, 120);
            else              push_rgb(0, 120, 120);
            vTaskDelay(pdMS_TO_TICKS(40));
            t += 40;
            break;
        }

        case LED_MODE_PING:
            /* Bright cyan flash for 200ms then revert. */
            push_rgb(0, 200, 200);
            vTaskDelay(pdMS_TO_TICKS(200));
            push_rgb(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
            s_mode = LED_MODE_IDLE;
            break;

        case LED_MODE_ZB_RX:
            /* Single white blip on a frame, then return to scan. */
            push_rgb(120, 120, 120);
            vTaskDelay(pdMS_TO_TICKS(35));
            s_mode = LED_MODE_SCAN;
            break;
        }
        (void)last_blip;
    }
}

void led_fx_init(void)
{
    rmt_tx_channel_config_t cfg = {
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .gpio_num        = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz   = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "rmt channel init failed (no LED on this board?)");
        return;
    }
    if (make_ws2812_encoder(&s_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "encoder init failed");
        return;
    }
    rmt_enable(s_chan);
    s_mode = LED_MODE_IDLE;
    xTaskCreate(fx_task, "led_fx", 3072, NULL, 2, &s_task);
    ESP_LOGI(TAG, "LED FX online on GPIO%d", LED_GPIO);
}

void led_fx_set(led_mode_t m) { s_mode = m; }
