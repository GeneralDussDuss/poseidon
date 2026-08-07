/*
 * leds_tembed - WS2812B ring driver + theme-driven animations for the
 * T-Embed CC1101's 8-LED rotary-encoder ring (GPIO 14, GRB order).
 *
 * FastLED was not present in .pio/libdeps/tembed (checked before writing
 * this), so the WS2812B bitstream is generated directly on the ESP32-S3's
 * RMT peripheral using the legacy driver/rmt.h API (rmt_config /
 * rmt_write_items). That header is present for esp32s3 in this project's
 * pinned framework-arduinoespressif32-libs build (idf-release_v5.5) —
 * it compiles with a deprecation #warning, not an error.
 *
 * Clock: legacy RMT defaults to the APB clock (80MHz) as its source; we
 * set clk_div=4 for a 50ns tick, giving:
 *   T0H 400ns -> 8 ticks   T0L 850ns -> 17 ticks
 *   T1H 800ns -> 16 ticks  T1L 450ns -> 9 ticks
 * all within WS2812B's documented tolerance. Reset (>50us low) is
 * satisfied by idle_output_en + idle_level LOW after each frame; frames
 * are spaced >=16ms apart by leds_tick()'s own rate limit, far longer
 * than the 50us floor.
 */
#if defined(POSEIDON_BOARD_TEMBED)

#include "leds_tembed.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <driver/rmt.h>

#include "board_tembed.h"
#include "../theme.h"

/* ================= WS2812B / RMT bitstream driver ================= */

#define WS_RMT_CHANNEL   RMT_CHANNEL_0
#define WS_TICK_T0H       8   /* 400ns @ 50ns/tick */
#define WS_TICK_T0L      17   /* 850ns */
#define WS_TICK_T1H      16   /* 800ns */
#define WS_TICK_T1L       9   /* 450ns */

static bool s_rmt_ready = false;

static void ws2812_rmt_init(void)
{
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX((gpio_num_t)TE_LED_PIN, WS_RMT_CHANNEL);
    config.clk_div = 4;   /* 50ns tick @ APB 80MHz */
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;
    if (rmt_config(&config) != ESP_OK) return;
    if (rmt_driver_install(config.channel, 0, 0) != ESP_OK) return;
    rmt_set_source_clk(WS_RMT_CHANNEL, RMT_BASECLK_APB);
    s_rmt_ready = true;
}

static inline void ws2812_bit_item(rmt_item32_t *item, bool one)
{
    item->level0    = 1;
    item->duration0 = one ? WS_TICK_T1H : WS_TICK_T0H;
    item->level1    = 0;
    item->duration1 = one ? WS_TICK_T1L : WS_TICK_T0L;
}

/* grb must hold TE_LED_COUNT * 3 bytes in G,R,B order per LED. */
static void ws2812_show(const uint8_t *grb)
{
    if (!s_rmt_ready) return;
    static rmt_item32_t items[TE_LED_COUNT * 24];
    size_t idx = 0;
    for (int led = 0; led < TE_LED_COUNT; led++) {
        for (int by = 0; by < 3; by++) {
            uint8_t v = grb[led * 3 + by];
            for (int bit = 7; bit >= 0; bit--) {
                ws2812_bit_item(&items[idx++], (v >> bit) & 0x1);
            }
        }
    }
    rmt_write_items(WS_RMT_CHANNEL, items, (int)idx, true);
    rmt_wait_tx_done(WS_RMT_CHANNEL, portMAX_DELAY);
}

/* ================= colour helpers ================= */

/* RGB565 -> RGB888, bit-replicated so 0xFFFF/0x0000 hit the full range. */
static inline void rgb565_to_888(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5)  & 0x3F;
    uint8_t b5 =  c        & 0x1F;
    r = (uint8_t)((r5 << 3) | (r5 >> 2));
    g = (uint8_t)((g6 << 2) | (g6 >> 4));
    b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline uint8_t scale8(uint8_t v, uint8_t scale)
{
    return (uint8_t)(((uint16_t)v * scale) / 255);
}

/* Rainbow-spin only — hue in [0,255], full saturation/value. */
static void hsv2rgb(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b)
{
    uint8_t region = h / 43;
    uint8_t rem    = (h - (region * 43)) * 6;
    uint8_t p = 0, q = (255 * (255 - rem)) >> 8, t = (255 * rem) >> 8;
    switch (region) {
    case 0:  r = 255; g = t;   b = p;   break;
    case 1:  r = q;   g = 255; b = p;   break;
    case 2:  r = p;   g = 255; b = t;   break;
    case 3:  r = p;   g = q;   b = 255; break;
    case 4:  r = t;   g = p;   b = 255; break;
    default: r = 255; g = p;   b = q;   break;
    }
}

static inline void put(uint8_t buf[TE_LED_COUNT][3], int i, uint8_t r, uint8_t g, uint8_t b, float frac)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    uint8_t s = (uint8_t)(frac * 255.0f + 0.5f);
    buf[i][0] = scale8(r, s);
    buf[i][1] = scale8(g, s);
    buf[i][2] = scale8(b, s);
}

/* Shortest signed distance from `led` to moving position `pos`, wrapped
 * into [0, TE_LED_COUNT) — used for trailing-tail renders. */
static inline float wrapped_delta(float pos, int led)
{
    float d = pos - led;
    while (d < 0) d += TE_LED_COUNT;
    while (d >= TE_LED_COUNT) d -= TE_LED_COUNT;
    return d;
}

/* ================= persistent-mode renderers ================= */

static void render_idle(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* ~3s breathing cycle, subtle: local intensity roughly 12%..65%. */
    uint8_t r, g, b;
    rgb565_to_888(T_ACCENT, r, g, b);
    float phase = fmodf(now, 3000.0f) / 3000.0f;
    float frac = 0.12f + 0.53f * (0.5f + 0.5f * sinf(2.0f * (float)M_PI * phase));
    for (int i = 0; i < TE_LED_COUNT; i++) put(buf, i, r, g, b, frac);
}

static void render_active(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Comet: bright head + 3-LED fading tail, ~2s/revolution. */
    uint8_t r, g, b;
    rgb565_to_888(T_ACCENT, r, g, b);
    const float tail_len = 3.0f;
    float pos = fmodf(now, 2000.0f) / 2000.0f * TE_LED_COUNT;
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float delta = wrapped_delta(pos, i);
        float frac = (delta < tail_len) ? (1.0f - delta / tail_len) : 0.0f;
        put(buf, i, r, g, b, frac);
    }
}

static void render_scan(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Radar sweep: fast orbit with a dim persistence trail. */
    uint8_t r, g, b;
    rgb565_to_888(T_ACCENT2, r, g, b);
    float pos = fmodf(now, 700.0f) / 700.0f * TE_LED_COUNT;
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float delta = wrapped_delta(pos, i);
        float rem = 1.0f - (delta / TE_LED_COUNT);
        float frac = (delta < 0.5f) ? 1.0f : (rem * rem * rem * rem * 0.35f);
        put(buf, i, r, g, b, frac);
    }
}

static void render_alert(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Strobe T_BAD / off at ~4Hz (250ms period). */
    uint8_t r, g, b;
    rgb565_to_888(T_BAD, r, g, b);
    bool on = (now % 250) < 125;
    for (int i = 0; i < TE_LED_COUNT; i++) put(buf, i, r, g, b, on ? 1.0f : 0.0f);
}

/* ================= one-shot event renderers ================= */
/* Each returns true while still running; false once expired (caller
 * then falls back to the persistent mode). */

static bool render_event_nav(uint32_t elapsed, int dir, uint8_t buf[TE_LED_COUNT][3])
{
    const uint32_t dur = 180;
    if (elapsed >= dur) return false;
    uint8_t r, g, b;
    rgb565_to_888(T_ACCENT2, r, g, b);
    float p = elapsed / (float)dur;
    float head = p * 3.0f;   /* pulse travels ~3 LEDs over the event */
    for (int i = 0; i < TE_LED_COUNT; i++) {
        int idx = (dir >= 0) ? i : (TE_LED_COUNT - 1 - i);
        float delta = head - idx;
        float frac = (delta < 0.0f || delta > 1.5f) ? 0.0f : (1.0f - delta / 1.5f);
        put(buf, i, r, g, b, frac);
    }
    return true;
}

static bool render_event_select(uint32_t elapsed, uint8_t buf[TE_LED_COUNT][3])
{
    const uint32_t dur = 300;
    if (elapsed >= dur) return false;
    uint8_t r, g, b;
    rgb565_to_888(T_GOOD, r, g, b);
    float p = elapsed / (float)dur;
    float frac = (1.0f - p);
    frac = frac * frac;   /* ease-out decay */
    for (int i = 0; i < TE_LED_COUNT; i++) put(buf, i, r, g, b, frac);
    return true;
}

static bool render_event_back(uint32_t elapsed, uint8_t buf[TE_LED_COUNT][3])
{
    const uint32_t dur = 220;
    if (elapsed >= dur) return false;
    uint8_t r, g, b;
    rgb565_to_888(T_DIM, r, g, b);
    float p = elapsed / (float)dur;
    float head = (1.0f - p) * 3.0f;   /* reverse sweep: runs backward across the ring */
    for (int i = 0; i < TE_LED_COUNT; i++) {
        int idx = TE_LED_COUNT - 1 - i;
        float delta = head - idx;
        float frac = (delta < 0.0f || delta > 1.5f) ? 0.0f : (1.0f - delta / 1.5f);
        put(buf, i, r, g, b, frac * 0.6f);   /* dimmer than nav */
    }
    return true;
}

static bool render_event_boot(uint32_t elapsed, uint8_t buf[TE_LED_COUNT][3])
{
    /* Rainbow spin-up, one full revolution — the one place we don't
     * pull from the theme, by design (splash flourish). */
    const uint32_t dur = 900;
    if (elapsed >= dur) return false;
    float p = elapsed / (float)dur;
    for (int i = 0; i < TE_LED_COUNT; i++) {
        uint8_t hue = (uint8_t)((i * (256 / TE_LED_COUNT)) + (uint8_t)(p * 256.0f));
        uint8_t r, g, b;
        hsv2rgb(hue, r, g, b);
        put(buf, i, r, g, b, 1.0f);
    }
    return true;
}

/* ================= public API / state machine ================= */

static led_mode_t   s_mode           = LED_MODE_IDLE;
static led_event_t  s_active_event   = LED_EVENT_SELECT;
static bool         s_event_pending  = false;
static uint32_t     s_event_start    = 0;
static uint32_t     s_last_update    = 0;
static uint8_t      s_brightness_cap = 40;   /* out of 255 */

void leds_begin(void)
{
    ws2812_rmt_init();
    s_mode = LED_MODE_IDLE;
    s_event_pending = false;
    s_last_update = 0;
    /* Push one all-off frame so the ring doesn't power up with
     * whatever garbage was left in the WS2812B's own latch. */
    uint8_t grb[TE_LED_COUNT * 3] = {0};
    ws2812_show(grb);
}

void leds_set_mode(led_mode_t m) { s_mode = m; }

void leds_event(led_event_t e)
{
    s_active_event  = e;
    s_event_start   = millis();
    s_event_pending = true;
}

void leds_set_brightness(uint8_t cap) { s_brightness_cap = cap; }

void leds_tick(void)
{
    uint32_t now = millis();
    if ((uint32_t)(now - s_last_update) < 16) return;
    s_last_update = now;

    uint8_t buf[TE_LED_COUNT][3];
    memset(buf, 0, sizeof(buf));

    bool event_active = false;
    if (s_event_pending) {
        uint32_t elapsed = now - s_event_start;
        switch (s_active_event) {
        case LED_EVENT_NAV_CW:  event_active = render_event_nav(elapsed, +1, buf); break;
        case LED_EVENT_NAV_CCW: event_active = render_event_nav(elapsed, -1, buf); break;
        case LED_EVENT_SELECT:  event_active = render_event_select(elapsed, buf);  break;
        case LED_EVENT_BACK:    event_active = render_event_back(elapsed, buf);    break;
        case LED_EVENT_BOOT:    event_active = render_event_boot(elapsed, buf);    break;
        }
        if (!event_active) s_event_pending = false;
    }

    if (!event_active) {
        switch (s_mode) {
        case LED_MODE_IDLE:   render_idle(now, buf);   break;
        case LED_MODE_ACTIVE: render_active(now, buf); break;
        case LED_MODE_SCAN:   render_scan(now, buf);   break;
        case LED_MODE_ALERT:  render_alert(now, buf);  break;
        }
    }

    uint8_t grb[TE_LED_COUNT * 3];
    for (int i = 0; i < TE_LED_COUNT; i++) {
        grb[i * 3 + 0] = scale8(buf[i][1], s_brightness_cap);   /* G */
        grb[i * 3 + 1] = scale8(buf[i][0], s_brightness_cap);   /* R */
        grb[i * 3 + 2] = scale8(buf[i][2], s_brightness_cap);   /* B */
    }
    ws2812_show(grb);
}

#endif /* POSEIDON_BOARD_TEMBED */
