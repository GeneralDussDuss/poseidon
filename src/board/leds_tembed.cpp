/*
 * leds_tembed - WS2812B ring driver + theme-driven animations for the
 * T-Embed CC1101's 8-LED rotary-encoder ring (GPIO 14, GRB order).
 *
 * FastLED was not present in .pio/libdeps/tembed, so the WS2812B bitstream
 * is generated directly on the ESP32-S3's RMT peripheral.
 *
 * MUST use the NEW RMT driver (driver/rmt_tx.h). An earlier version of this
 * file used the legacy driver/rmt.h API and bricked every boot:
 *   E rmt(legacy): CONFLICT! driver_ng is not allowed to be used with the
 *   legacy driver  -> abort() -> reboot loop, black screen.
 * ESP-IDF permits exactly one of the two APIs per firmware image, and
 * src/cc1101_rmt.cpp and src/features/ir_learn.cpp already use the new one.
 *
 * Resolution 10MHz gives a 100ns tick:
 *   T0H 400ns -> 4 ticks   T0L 850ns -> 8 ticks
 *   T1H 800ns -> 8 ticks   T1L 450ns -> 5 ticks
 * all inside WS2812B tolerance. The >50us reset gap is satisfied by
 * leds_tick()'s own >=16ms frame spacing.
 */
#if defined(POSEIDON_BOARD_TEMBED)

#include "leds_tembed.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>
#include <driver/rmt_tx.h>

#include "board_tembed.h"
#include "../theme.h"

/* ================= WS2812B / RMT bitstream driver ================= */

#define WS_RESOLUTION_HZ 10000000  /* 100ns per tick */
#define WS_TICK_T0H       4        /* 400ns */
#define WS_TICK_T0L       8        /* 850ns */
#define WS_TICK_T1H       8        /* 800ns */
#define WS_TICK_T1L       5        /* 450ns */

static bool                  s_rmt_ready = false;
static rmt_channel_handle_t  s_chan      = nullptr;
static rmt_encoder_handle_t  s_encoder   = nullptr;

static void ws2812_rmt_init(void)
{
    /* An ESP32-S3 RMT channel owns only 48 symbol words of hardware memory,
     * but one WS2812B frame here is 8 LEDs x 24 bits = 192 symbols. Without
     * DMA the driver has to refill the block mid-transmission from an ISR,
     * and any late refill stretches a bit period past WS2812B tolerance, so
     * the strip latches garbage (typically full white). Ask for DMA with a
     * buffer big enough for the whole frame, and fall back to the non-DMA
     * path if this chip refuses. */
    rmt_tx_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num          = (gpio_num_t)TE_LED_PIN;
    ch_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    ch_cfg.resolution_hz     = WS_RESOLUTION_HZ;
    ch_cfg.mem_block_symbols = TE_LED_COUNT * 24;   /* 192, whole frame */
    ch_cfg.trans_queue_depth = 4;
    ch_cfg.flags.with_dma    = 1;
    esp_err_t e = rmt_new_tx_channel(&ch_cfg, &s_chan);
    if (e != ESP_OK) {
        Serial.printf("[leds] DMA channel FAILED (%s), retrying without DMA\n",
                      esp_err_to_name(e));
        ch_cfg.flags.with_dma    = 0;
        ch_cfg.mem_block_symbols = 48;   /* hardware max per S3 channel */
        e = rmt_new_tx_channel(&ch_cfg, &s_chan);
    }
    if (e != ESP_OK) {
        Serial.printf("[leds] rmt_new_tx_channel FAILED: %s\n", esp_err_to_name(e));
        return;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    e = rmt_new_copy_encoder(&enc_cfg, &s_encoder);
    if (e != ESP_OK) {
        Serial.printf("[leds] rmt_new_copy_encoder FAILED: %s\n", esp_err_to_name(e));
        rmt_del_channel(s_chan);
        s_chan = nullptr;
        return;
    }
    e = rmt_enable(s_chan);
    if (e != ESP_OK) {
        Serial.printf("[leds] rmt_enable FAILED: %s\n", esp_err_to_name(e));
        return;
    }
    s_rmt_ready = true;
    Serial.printf("[leds] RMT ready on GPIO %d, %d LEDs, dma=%d\n",
                  TE_LED_PIN, TE_LED_COUNT, (int)ch_cfg.flags.with_dma);
}

static inline void ws2812_bit_item(rmt_symbol_word_t *item, bool one)
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
    static rmt_symbol_word_t items[TE_LED_COUNT * 24];
    size_t idx = 0;
    for (int led = 0; led < TE_LED_COUNT; led++) {
        for (int by = 0; by < 3; by++) {
            uint8_t v = grb[led * 3 + by];
            for (int bit = 7; bit >= 0; bit--) {
                ws2812_bit_item(&items[idx++], (v >> bit) & 0x1);
            }
        }
    }
    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    if (rmt_transmit(s_chan, s_encoder, items,
                     idx * sizeof(rmt_symbol_word_t), &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 100);
    }
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

/* Additive blend into buf[i] (accumulate + clamp), for effects that layer
 * more than one moving light over the same pixel (e.g. two comets meeting). */
static inline void add(uint8_t buf[TE_LED_COUNT][3], int i, uint8_t r, uint8_t g, uint8_t b, float frac)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    uint8_t s = (uint8_t)(frac * 255.0f + 0.5f);
    uint16_t nr = (uint16_t)buf[i][0] + scale8(r, s);
    uint16_t ng = (uint16_t)buf[i][1] + scale8(g, s);
    uint16_t nb = (uint16_t)buf[i][2] + scale8(b, s);
    buf[i][0] = (nr > 255) ? 255 : (uint8_t)nr;
    buf[i][1] = (ng > 255) ? 255 : (uint8_t)ng;
    buf[i][2] = (nb > 255) ? 255 : (uint8_t)nb;
}

/* Linear interpolate one colour byte a->b by t in [0,1]. */
static inline uint8_t lerp8(uint8_t a, uint8_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (uint8_t)((float)a + ((float)b - (float)a) * t + 0.5f);
}

/* ================= gamma + sine lookup tables =================
 * Both are built once at boot (leds_begin), never touched again from the
 * hot path. leds_tick() only ever indexes into them -- no per-frame
 * powf()/sinf() calls. */

static uint8_t s_gamma[256];

static void build_gamma_table(void)
{
    /* WS2812 output is linear in PWM duty but the eye is not: without
     * this, fades look harsh and top-heavy (most of the visible change
     * crammed into the last 20% of the range). gamma ~2.6 spreads the
     * low end out so a breath/decay reads as smooth. */
    const float GAMMA = 2.6f;
    for (int i = 0; i < 256; i++) {
        float v = powf((float)i / 255.0f, GAMMA);
        s_gamma[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
}

#define SIN_LUT_SIZE 32   /* power of two -> index via mask, no modulo */
static uint8_t s_sine_lut[SIN_LUT_SIZE];

static void build_sine_lut(void)
{
    for (int i = 0; i < SIN_LUT_SIZE; i++) {
        float v = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * i / SIN_LUT_SIZE);
        s_sine_lut[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
}

/* phase01 wraps to [0,1) -> 0..1 sine-shaped value, LUT + one lerp-free
 * index, no trig in the per-frame path. */
static inline float lut_sin01(float phase01)
{
    phase01 = phase01 - floorf(phase01);
    int idx = (int)(phase01 * SIN_LUT_SIZE) & (SIN_LUT_SIZE - 1);
    return s_sine_lut[idx] / 255.0f;
}

/* Deterministic per-LED twinkle. Hashes (led, time-bucket) so glints land at
 * pseudo-random positions but stay stable WITHIN a bucket -- re-rolling every
 * frame would just look like noise. Returns a 1..0 fade across the bucket so a
 * glint decays instead of popping off. */
static inline float sparkle(int i, uint32_t now, uint32_t bucket_ms, float chance)
{
    uint32_t h = (uint32_t)i * 2654435761u ^ ((now / bucket_ms) * 2246822519u);
    h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    if ((h & 0xFFu) > (uint32_t)(chance * 255.0f)) return 0.0f;
    const float t = (float)(now % bucket_ms) / (float)bucket_ms;
    return 1.0f - t;
}


/* ================= radio-driven state ================= */
/* Set via leds_set_rssi()/leds_set_channel()/leds_packet_hit(). All start
 * at safe, inert defaults so a ring nobody wires up never glitches. */

#define LED_RSSI_FLOOR (-90)
#define LED_RSSI_CEIL  (-30)

static int8_t   s_rssi_dbm       = LED_RSSI_FLOOR;   /* weak by default */
static uint8_t  s_wifi_channel   = 1;
static uint8_t  s_traffic_energy[TE_LED_COUNT] = {0};
static uint8_t  s_traffic_pos    = 0;   /* last position a packet hit landed on */
static uint32_t s_traffic_last_ms = 0;

/* ================= persistent-mode renderers ================= */

static void render_idle(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Aurora: three colour stops drifting at non-multiple periods so the ring
     * never resyncs into a visible loop, over a slow global "breath" that makes
     * the whole ring swell and ebb. Occasional white glints drift across it.
     *
     * The previous version peaked at 0.20 and, under the old 40/255 cap, landed
     * near 3% output -- technically animated, practically invisible. */
    uint8_t ar, ag, ab, br, bg, bb;
    rgb565_to_888(T_ACCENT,  ar, ag, ab);
    rgb565_to_888(T_ACCENT2, br, bg, bb);

    const float pa = 5200.0f, pb = 7300.0f, pc = 11100.0f;
    const float phase_a = fmodf((float)now, pa) / pa;
    const float phase_b = fmodf((float)now, pb) / pb;
    const float phase_c = fmodf((float)now, pc) / pc;

    /* Global breath, 0.55..1.0, so quiet moments still read as lit. */
    const float breath = 0.55f + 0.45f * lut_sin01(phase_c);

    for (int i = 0; i < TE_LED_COUNT; i++) {
        const float posf = (float)i / (float)TE_LED_COUNT;
        const float la = lut_sin01(posf + phase_a);
        const float lb = lut_sin01(posf * 2.0f + phase_b + 0.5f);

        add(buf, i, ar, ag, ab, (0.10f + 0.45f * la * la) * breath);
        add(buf, i, br, bg, bb, (0.08f + 0.38f * lb * lb) * breath);

        /* Cool white glint, rare and short. */
        const float sp = sparkle(i, now, 900, 0.10f);
        if (sp > 0.0f) add(buf, i, 200, 235, 255, 0.55f * sp * sp);
    }
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
    /* Two counter-rotating radar sweeps at different rates, each with a hot
     * near-white core and a persistence trail. Where they cross they add and
     * flare, which gives the ring an irregular pulse that reads as "actively
     * hunting" rather than a single dot going round. */
    uint8_t r1, g1, b1, r2, g2, b2;
    rgb565_to_888(T_ACCENT2, r1, g1, b1);
    rgb565_to_888(T_ACCENT,  r2, g2, b2);

    const float pos_a = fmodf(now, 700.0f)  / 700.0f  * TE_LED_COUNT;
    const float pos_b = TE_LED_COUNT - fmodf(now, 1100.0f) / 1100.0f * TE_LED_COUNT;

    for (int i = 0; i < TE_LED_COUNT; i++) {
        const float da = wrapped_delta(pos_a, i);
        const float db = wrapped_delta(pos_b, i);

        float fa = 1.0f - (da / TE_LED_COUNT);
        fa = (da < 0.6f) ? 1.0f : fa * fa * fa * fa * 0.45f;
        float fb = 1.0f - (db / TE_LED_COUNT);
        fb = (db < 0.6f) ? 0.8f : fb * fb * fb * fb * 0.30f;

        add(buf, i, r1, g1, b1, fa);
        add(buf, i, r2, g2, b2, fb);
        if (da < 0.5f) add(buf, i, 180, 220, 255, 0.5f);   /* hot core */
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

static void render_rssi(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Signal-strength meter: leds_set_rssi(). Fill length AND colour heat
     * both track proximity (T_DIM -> T_ACCENT -> T_GOOD), plus a pulse
     * that quickens the closer the target gets -- a heartbeat you feel
     * without reading a dBm number off the screen. */
    float n = (float)(s_rssi_dbm - LED_RSSI_FLOOR) / (float)(LED_RSSI_CEIL - LED_RSSI_FLOOR);
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;

    uint8_t dr, dg, db; rgb565_to_888(T_DIM, dr, dg, db);
    uint8_t ar, ag, ab; rgb565_to_888(T_ACCENT, ar, ag, ab);
    uint8_t gr, gg, gb; rgb565_to_888(T_GOOD, gr, gg, gb);
    uint8_t r, g, b;
    if (n < 0.5f) {
        float t = n * 2.0f;
        r = lerp8(dr, ar, t); g = lerp8(dg, ag, t); b = lerp8(db, ab, t);
    } else {
        float t = (n - 0.5f) * 2.0f;
        r = lerp8(ar, gr, t); g = lerp8(ag, gg, t); b = lerp8(ab, gb, t);
    }

    float period = 1400.0f - 900.0f * n;   /* far = slow pulse, close = fast */
    float pulse = 0.72f + 0.28f * lut_sin01(fmodf((float)now, period) / period);

    float fill = n * (float)TE_LED_COUNT;
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float frac = fill - (float)i;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        put(buf, i, r, g, b, frac * pulse);
    }
}

static void render_channel(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Ring position encodes WiFi channel via leds_set_channel(). Everything
     * but the indicated arc stays dim; the arc itself blends colour AND
     * brightness across neighbouring LEDs so adjacent channels are visibly
     * distinct even though there are only 8 pixels for 14 channels. */
    (void)now;
    uint8_t ch = s_wifi_channel;
    if (ch < 1) ch = 1;
    if (ch > 14) ch = 14;
    float pos = (float)(ch - 1) * (float)(TE_LED_COUNT - 1) / 13.0f;

    uint8_t dr, dg, db; rgb565_to_888(T_DIM, dr, dg, db);
    uint8_t ar, ag, ab; rgb565_to_888(T_ACCENT2, ar, ag, ab);
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float d = fabsf((float)i - pos);
        float frac = (d < 1.3f) ? (1.0f - d / 1.3f) : 0.0f;
        uint8_t r = lerp8(dr, ar, frac);
        uint8_t g = lerp8(dg, ag, frac);
        uint8_t b = lerp8(db, ab, frac);
        put(buf, i, r, g, b, 0.15f + 0.85f * frac);
    }
}

static void render_traffic(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Live packet monitor: leds_packet_hit() injects energy at a rotating
     * position (step of 3, coprime with 8, so eight hits scatter across
     * all eight LEDs instead of clustering); each frame decays every
     * pixel by elapsed wall-clock time, not a fixed per-tick amount, so
     * the fade rate doesn't drift with frame rate. Idle == near-dark;
     * a busy channel == a storm, hot pixels shifting toward T_WARN. */
    uint32_t dt = (s_traffic_last_ms == 0) ? 0 : (now - s_traffic_last_ms);
    s_traffic_last_ms = now;
    if (dt > 200) dt = 200;   /* clamp huge gaps (mode just switched in) */
    uint16_t decay = (uint16_t)((dt * 60) / 100);   /* ~600/s -> ~425ms full fade */

    uint8_t r, g, b;   rgb565_to_888(T_ACCENT2, r, g, b);
    uint8_t wr, wg, wb; rgb565_to_888(T_WARN, wr, wg, wb);
    for (int i = 0; i < TE_LED_COUNT; i++) {
        uint8_t e = s_traffic_energy[i];
        e = (e > decay) ? (uint8_t)(e - decay) : 0;
        s_traffic_energy[i] = e;
        float level = e / 255.0f;
        uint8_t rr = lerp8(r, wr, level);
        uint8_t gg = lerp8(g, wg, level);
        uint8_t bb = lerp8(b, wb, level);
        put(buf, i, rr, gg, bb, 0.05f + level * 0.95f);
    }
}

static void render_attack(uint32_t now, uint8_t buf[TE_LED_COUNT][3])
{
    /* Two counter-rotating comets, deliberately fast and aggressive, for
     * deauth/flood screens. They cross twice a revolution and add (not
     * overwrite) where they overlap, so the crossing points flare hot. */
    const float period = 450.0f;
    const float tail = 2.2f;
    float pos_a = fmodf((float)now, period) / period * (float)TE_LED_COUNT;
    float pos_b = (float)TE_LED_COUNT - pos_a;
    if (pos_b >= (float)TE_LED_COUNT) pos_b -= (float)TE_LED_COUNT;

    uint8_t wr, wg, wb; rgb565_to_888(T_WARN, wr, wg, wb);
    uint8_t br, bg, bb; rgb565_to_888(T_BAD, br, bg, bb);
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float da = wrapped_delta(pos_a, i);
        float fa = (da < tail) ? (1.0f - da / tail) : 0.0f;
        float dbp = wrapped_delta(pos_b, i);
        float fb = (dbp < tail) ? (1.0f - dbp / tail) : 0.0f;
        add(buf, i, wr, wg, wb, fa);
        add(buf, i, br, bg, bb, fb);
    }

    /* Random hot flares over the comets: an attack screen should look
     * unmistakably different from a scan at a glance across a room. */
    for (int i = 0; i < TE_LED_COUNT; i++) {
        const float sp = sparkle(i, now, 140, 0.35f);
        if (sp > 0.0f) add(buf, i, 255, 240, 210, 0.8f * sp);
    }
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

static bool render_event_hit(uint32_t elapsed, uint8_t buf[TE_LED_COUNT][3])
{
    /* Capture/handshake landed -- should feel like a reward. A bright
     * flash expands from the last traffic-hit position outward in both
     * directions around the ring, then resolves into a full-ring T_GOOD
     * pulse that eases out. */
    const uint32_t dur1 = 180, dur2 = 250, dur = dur1 + dur2;
    if (elapsed >= dur) return false;
    uint8_t r, g, b;
    rgb565_to_888(T_GOOD, r, g, b);

    if (elapsed < dur1) {
        float p = (float)elapsed / (float)dur1;
        float reach = p * ((float)TE_LED_COUNT / 2.0f + 1.0f);
        int seed = s_traffic_pos;
        for (int i = 0; i < TE_LED_COUNT; i++) {
            int d = i - seed;
            if (d > TE_LED_COUNT / 2) d -= TE_LED_COUNT;
            if (d < -TE_LED_COUNT / 2) d += TE_LED_COUNT;
            int ad = (d < 0) ? -d : d;
            float frac = ((float)ad <= reach) ? 1.0f : 0.0f;
            put(buf, i, r, g, b, frac);
        }
    } else {
        float p = (float)(elapsed - dur1) / (float)dur2;
        float frac = 1.0f - p;
        frac = frac * frac;   /* ease-out decay, same shape as SELECT */
        for (int i = 0; i < TE_LED_COUNT; i++) put(buf, i, r, g, b, frac);
    }
    return true;
}

static bool render_event_alert_spin(uint32_t elapsed, uint8_t buf[TE_LED_COUNT][3])
{
    /* Fast full-ring spin in T_BAD for detection/alert screens -- ~4 rev/s,
     * overall envelope fades across the event so it doesn't cut off hard. */
    const uint32_t dur = 600;
    if (elapsed >= dur) return false;
    uint8_t r, g, b;
    rgb565_to_888(T_BAD, r, g, b);
    const float rev_period = 150.0f;
    float pos = fmodf((float)elapsed, rev_period) / rev_period * (float)TE_LED_COUNT;
    float envelope = 1.0f - (float)elapsed / (float)dur;
    for (int i = 0; i < TE_LED_COUNT; i++) {
        float delta = wrapped_delta(pos, i);
        float frac = (delta < 1.5f) ? (1.0f - delta / 1.5f) : 0.0f;
        put(buf, i, r, g, b, frac * envelope);
    }
    return true;
}

/* ================= public API / state machine ================= */

static led_mode_t   s_mode           = LED_MODE_IDLE;
static led_event_t  s_active_event   = LED_EVENT_SELECT;
static bool         s_event_pending  = false;
static uint32_t     s_event_start    = 0;
static uint32_t     s_last_update    = 0;
/* Out of 255. This was 40, which combined with idle's 0.20 peak put the ring at
 * roughly 3% actual output -- present, but almost invisible in daylight. 110
 * still reads as ambient rather than a torch, and gives the brighter modes
 * (attack, alert, hit) somewhere to actually punch to. */
static uint8_t      s_brightness_cap = 110;

/* Previous rendered frame (pre-brightness-cap, pre-gamma), used for the
 * motion-smear blend below. Static storage -> zero-initialized, so the
 * ring simply fades in from black across the first couple of frames at
 * boot rather than reading uninitialized memory. */
static uint8_t s_prev[TE_LED_COUNT][3];

/* The ring must animate no matter which screen is up. Driving leds_tick()
 * from a UI loop only works while that particular loop is running: enter any
 * feature, or the terminal menu, and the ring freezes on its last frame,
 * which is exactly what "stuck on cyan" was. A tiny dedicated task keeps it
 * alive everywhere, costs ~1% of a core at 60Hz, and means no future screen
 * has to remember to pump it. */
static void leds_task(void *)
{
    for (;;) {
        leds_tick();
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void leds_begin(void)
{
    build_gamma_table();
    build_sine_lut();
    ws2812_rmt_init();
    s_mode = LED_MODE_IDLE;
    s_event_pending = false;
    s_last_update = 0;
    memset(s_prev, 0, sizeof(s_prev));
    memset(s_traffic_energy, 0, sizeof(s_traffic_energy));
    s_traffic_pos = 0;
    s_traffic_last_ms = 0;
    /* Push one all-off frame so the ring doesn't power up with
     * whatever garbage was left in the WS2812B's own latch. */
    uint8_t grb[TE_LED_COUNT * 3] = {0};
    ws2812_show(grb);

    static TaskHandle_t s_task = nullptr;
    if (s_rmt_ready && s_task == nullptr) {
        xTaskCreatePinnedToCore(leds_task, "leds", 2048, nullptr, 1, &s_task, 1);
    }
}

void leds_set_mode(led_mode_t m) { s_mode = m; }

void leds_event(led_event_t e)
{
    s_active_event  = e;
    s_event_start   = millis();
    s_event_pending = true;
}

void leds_set_brightness(uint8_t cap) { s_brightness_cap = cap; }

void leds_set_rssi(int8_t dbm) { s_rssi_dbm = dbm; }

void leds_set_channel(uint8_t ch)
{
    if (ch < 1) ch = 1;
    if (ch > 14) ch = 14;
    s_wifi_channel = ch;
}

void leds_packet_hit(void)
{
    /* +3 is coprime with TE_LED_COUNT(8), so 8 consecutive hits visit
     * every LED exactly once in a scattered, non-adjacent order instead
     * of just marching around the ring. */
    s_traffic_pos = (uint8_t)((s_traffic_pos + 3) % TE_LED_COUNT);
    s_traffic_energy[s_traffic_pos] = 255;
}

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
        case LED_EVENT_NAV_CW:     event_active = render_event_nav(elapsed, +1, buf); break;
        case LED_EVENT_NAV_CCW:    event_active = render_event_nav(elapsed, -1, buf); break;
        case LED_EVENT_SELECT:     event_active = render_event_select(elapsed, buf);  break;
        case LED_EVENT_BACK:       event_active = render_event_back(elapsed, buf);    break;
        case LED_EVENT_BOOT:       event_active = render_event_boot(elapsed, buf);    break;
        case LED_EVENT_HIT:        event_active = render_event_hit(elapsed, buf);     break;
        case LED_EVENT_ALERT_SPIN: event_active = render_event_alert_spin(elapsed, buf); break;
        }
        if (!event_active) s_event_pending = false;
    }

    /* Sweeps and breathing get motion smear (phosphor-style persistence);
     * strobes, hits, and every one-shot event stay snappy so they still
     * read as sharp reactions instead of getting blurred into the mush. */
    bool smear = false;

    if (!event_active) {
        switch (s_mode) {
        case LED_MODE_IDLE:    render_idle(now, buf);    smear = true;  break;
        case LED_MODE_ACTIVE:  render_active(now, buf);  smear = true;  break;
        case LED_MODE_SCAN:    render_scan(now, buf);    smear = true;  break;
        case LED_MODE_ALERT:   render_alert(now, buf);   smear = false; break;
        case LED_MODE_RSSI:    render_rssi(now, buf);    smear = true;  break;
        case LED_MODE_CHANNEL: render_channel(now, buf); smear = true;  break;
        case LED_MODE_TRAFFIC: render_traffic(now, buf); smear = false; break;
        case LED_MODE_ATTACK:  render_attack(now, buf);  smear = false; break;
        }
    }

    if (smear) {
        /* Recursive low-pass (roughly 40% new / 60% old each frame) against
         * the previous frame -- because s_prev itself already carries the
         * prior blend, this decays like phosphor across many frames, not
         * just a single-frame cross-fade. */
        for (int i = 0; i < TE_LED_COUNT; i++) {
            for (int c = 0; c < 3; c++) {
                uint16_t blended = (uint16_t)buf[i][c] * 102 + (uint16_t)s_prev[i][c] * 153;
                buf[i][c] = (uint8_t)(blended / 255);
            }
        }
    }
    memcpy(s_prev, buf, sizeof(s_prev));

    uint8_t grb[TE_LED_COUNT * 3];
    for (int i = 0; i < TE_LED_COUNT; i++) {
        uint8_t g = scale8(buf[i][1], s_brightness_cap);
        uint8_t r = scale8(buf[i][0], s_brightness_cap);
        uint8_t b = scale8(buf[i][2], s_brightness_cap);
        grb[i * 3 + 0] = s_gamma[g];
        grb[i * 3 + 1] = s_gamma[r];
        grb[i * 3 + 2] = s_gamma[b];
    }
    ws2812_show(grb);
}

#endif /* POSEIDON_BOARD_TEMBED */
