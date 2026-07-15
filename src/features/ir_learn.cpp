/*
 * ir_learn — capture a raw IR frame from a real remote on the hat's receiver
 * (IR_RX_PIN) using the ESP32-S3 RMT RX peripheral, then replay it out the
 * hat's emitter (IR_TX_PIN) via a software 38 kHz bit-bang. Raw timings only
 * (no protocol decode). Optional save to /poseidon/ir/*.ir.
 *
 * RMT RX config mirrors cc1101_rmt.cpp (1 µs ticks, 64-symbol block, no DMA).
 * A 64-symbol block holds up to 128 edges — enough for typical consumer
 * remotes; longer frames (e.g. A/C units) truncate, flagged to the user.
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "../ir_hw.h"
#include "../ir_learn_decode.h"
#include "../sd_helper.h"
#include <driver/rmt_rx.h>
#include <driver/gpio.h>
#include <SD.h>

#define IR_MAX_EDGES 512
#define RMT_SYMS     64

static uint16_t s_timings[IR_MAX_EDGES];
static uint16_t s_count = 0;
static bool     s_truncated = false;

/* ---- RMT RX capture ---- */
static rmt_symbol_word_t s_raw[RMT_SYMS];
static volatile bool     s_rx_done = false;
static volatile size_t   s_rx_num  = 0;

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t, const rmt_rx_done_event_data_t *ed, void *) {
    s_rx_num  = ed->num_symbols;
    s_rx_done = true;
    return false;
}

/* Arm RMT RX and wait up to timeout_ms for a frame. Returns edge count (0 = none). */
static uint16_t ir_capture(uint32_t timeout_ms) {
    rmt_channel_handle_t rx = nullptr;
    rmt_rx_channel_config_t cfg = {};
    cfg.gpio_num          = (gpio_num_t)IR_RX_PIN;
    cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz     = 1000000;       /* 1 µs ticks */
    cfg.mem_block_symbols = RMT_SYMS;
    if (rmt_new_rx_channel(&cfg, &rx) != ESP_OK) return 0;

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rx_done_cb;
    rmt_rx_register_event_callbacks(rx, &cbs, nullptr);
    rmt_enable(rx);

    rmt_receive_config_t rc = {};
    rc.signal_range_min_ns = 2000;         /* 2 µs glitch filter */
    rc.signal_range_max_ns = 12000000;     /* 12 ms idle = end of frame */
    s_rx_done = false; s_rx_num = 0;
    rmt_receive(rx, s_raw, sizeof(s_raw), &rc);

    uint32_t end = millis() + timeout_ms;
    while (!s_rx_done && millis() < end) {
        if (input_poll() == PK_ESC) break;
        delay(10);
    }
    uint16_t n = 0;
    if (s_rx_done) {
        ir_edge_pair_t pairs[RMT_SYMS];
        size_t ns = s_rx_num < RMT_SYMS ? s_rx_num : RMT_SYMS;
        for (size_t i = 0; i < ns; ++i) {
            pairs[i].d0 = s_raw[i].duration0; pairs[i].l0 = s_raw[i].level0;
            pairs[i].d1 = s_raw[i].duration1; pairs[i].l1 = s_raw[i].level1;
        }
        n = ir_symbols_to_us(pairs, ns, s_timings, IR_MAX_EDGES, &s_truncated);
    }
    rmt_disable(rx);
    rmt_del_channel(rx);
    return n;
}

/* ---- software 38 kHz replay on IR_TX_PIN ---- */
#if IR_TX_ACTIVE_LOW
#  define TX_ON  LOW
#  define TX_OFF HIGH
#else
#  define TX_ON  HIGH
#  define TX_OFF LOW
#endif
static const int TX_HALF_US = 13;   /* 38 kHz half period */

static void ir_replay(void) {
    gpio_reset_pin((gpio_num_t)IR_TX_PIN);
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, TX_OFF);
    for (uint16_t i = 0; i < s_count; ++i) {
        uint16_t us = s_timings[i];
        if (i & 1) {                          /* odd index = space */
            digitalWrite(IR_TX_PIN, TX_OFF);
            if (us) delayMicroseconds(us);
        } else {                              /* even index = mark (carrier) */
            uint32_t stop = micros() + us;
            while ((int32_t)(stop - micros()) > 0) {
                digitalWrite(IR_TX_PIN, TX_ON);  delayMicroseconds(TX_HALF_US);
                digitalWrite(IR_TX_PIN, TX_OFF); delayMicroseconds(TX_HALF_US);
            }
        }
    }
    digitalWrite(IR_TX_PIN, TX_OFF);
}

/* ---- SD save (/poseidon/ir/slot<N>.ir : CSV of µs values) ---- */
static void ir_save(int slot) {
    if (!sd_mount()) { ui_toast("no SD", T_BAD, 1000); return; }
    SD.mkdir("/poseidon"); SD.mkdir("/poseidon/ir");
    char path[40]; snprintf(path, sizeof(path), "/poseidon/ir/slot%d.ir", slot);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { ui_toast("save failed", T_BAD, 1000); return; }
    for (uint16_t i = 0; i < s_count; ++i) f.printf("%u%s", s_timings[i], i + 1 < s_count ? "," : "\n");
    f.close();
    ui_toast("saved", T_GOOD, 800);
}

void feat_ir_learn(void) {
    int slot = 0;
    ui_clear_body();
    ui_draw_status("IR", "learn");
    ui_draw_footer("SPACE=capture  R=replay  S=save  `=back");
    auto &d = M5Cardputer.Display;
    d.setTextColor(T_ACCENT, T_BG);
    d.setCursor(4, BODY_Y + 2); d.print("IR LEARN");
    d.setTextColor(T_DIM, T_BG);
    d.setCursor(4, BODY_Y + 20); d.print("point remote, press SPACE");

    while (true) {
        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) return;
        if (k == PK_SPACE) {
            d.fillRect(0, BODY_Y + 34, SCR_W, 40, T_BG);
            d.setTextColor(T_WARN, T_BG);
            d.setCursor(4, BODY_Y + 34); d.print("waiting for signal...");
            uint16_t n = ir_capture(6000);
            s_count = n;
            d.fillRect(0, BODY_Y + 34, SCR_W, 40, T_BG);
            if (n == 0) {
                d.setTextColor(T_BAD, T_BG);
                d.setCursor(4, BODY_Y + 34); d.print("no signal");
            } else {
                d.setTextColor(T_GOOD, T_BG);
                d.setCursor(4, BODY_Y + 34);
                d.printf("captured %u edges%s", n, s_truncated ? " (trunc)" : "");
            }
        } else if ((k == 'r' || k == 'R') && s_count > 0) {
            ir_replay();
            ui_toast("replayed", T_ACCENT, 500);
        } else if ((k == 's' || k == 'S') && s_count > 0) {
            ir_save(slot++);
        }
    }
}
