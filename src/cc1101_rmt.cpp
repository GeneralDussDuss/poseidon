/*
 * cc1101_rmt.cpp — new-API RMT driver (see header).
 *
 * ESP-IDF 5.x RMT model:
 *   - Each channel is allocated with rmt_new_tx_channel / rmt_new_rx_channel.
 *   - TX uses an encoder; we use the built-in copy encoder since our
 *     rmt_symbol_word_t array is pre-shaped correctly.
 *   - RX callback fires in ISR context; we signal a FreeRTOS task
 *     notification to wake the caller.
 *
 * Resolution: 1 MHz (1 µs per tick) matches Flipper .sub file encoding
 * directly — durations transcribe 1:1.
 *
 * Memory: RMT symbol block is 64 symbols × 4 bytes = 256 bytes per
 * channel. We allocate channels on demand and free on exit so WiFi /
 * BLE radios can reclaim the RAM.
 */
#include "cc1101_rmt.h"
#include "cc1101_hw.h"
#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define RMT_RESOLUTION_HZ   1000000   /* 1 MHz → 1 µs tick */
#define RMT_MEM_SYMBOLS     64

/* --- TX ---------------------------------------------------------------- */

bool cc1101_rmt_tx(const int16_t *pulses, int n_pulses)
{
    if (!pulses || n_pulses <= 0) return false;

    /* Each RMT symbol packs TWO pulses (level0/duration0 + level1/duration1).
     * Ceiling divide, the last symbol's second half is padded if odd. */
    int n_sym = (n_pulses + 1) / 2;
    rmt_symbol_word_t *syms = (rmt_symbol_word_t *)calloc(n_sym, sizeof(rmt_symbol_word_t));
    if (!syms) return false;

    for (int i = 0; i < n_pulses; ++i) {
        int16_t v = pulses[i];
        uint16_t dur = (uint16_t)(v < 0 ? -v : v);
        if (dur > 0x7FFF) dur = 0x7FFF;   /* 15-bit field cap at ~32.7 ms */
        uint8_t  lvl = v > 0 ? 1 : 0;
        rmt_symbol_word_t &s = syms[i / 2];
        if ((i & 1) == 0) {
            s.level0    = lvl;
            s.duration0 = dur;
        } else {
            s.level1    = lvl;
            s.duration1 = dur;
        }
    }
    /* If odd count, trailing half-symbol needs a zero duration pad at the
     * same level as its level0 (avoids a phantom level flip at end). */
    if (n_pulses & 1) {
        rmt_symbol_word_t &last = syms[n_sym - 1];
        last.level1    = last.level0;
        last.duration1 = 0;
    }

    rmt_channel_handle_t  tx_chan     = nullptr;
    rmt_encoder_handle_t  copy_enc    = nullptr;
    bool ok = true;

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num          = (gpio_num_t)CC1101_GDO0;
    tx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz     = RMT_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = RMT_MEM_SYMBOLS;
    tx_cfg.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&tx_cfg, &tx_chan) != ESP_OK) { ok = false; goto done; }

    {
        rmt_copy_encoder_config_t cpy_cfg = {};
        if (rmt_new_copy_encoder(&cpy_cfg, &copy_enc) != ESP_OK) { ok = false; goto done; }
    }

    if (rmt_enable(tx_chan) != ESP_OK) { ok = false; goto done; }

    {
        rmt_transmit_config_t xmit_cfg = {};
        xmit_cfg.loop_count = 0;
        if (rmt_transmit(tx_chan, copy_enc, syms,
                         n_sym * sizeof(rmt_symbol_word_t), &xmit_cfg) != ESP_OK) {
            ok = false; goto done;
        }
        rmt_tx_wait_all_done(tx_chan, pdMS_TO_TICKS(5000));
    }

done:
    if (tx_chan) {
        rmt_disable(tx_chan);
        rmt_del_channel(tx_chan);
    }
    if (copy_enc) rmt_del_encoder(copy_enc);
    free(syms);
    return ok;
}

/* --- RX ---------------------------------------------------------------- */

struct rx_ctx_t {
    TaskHandle_t waiter;
    int          n_symbols;
};

/* ISR context — just signal the waiting task with the symbol count. */
static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t ch,
                                 const rmt_rx_done_event_data_t *edata,
                                 void *user_ctx)
{
    (void)ch;
    rx_ctx_t *ctx = (rx_ctx_t *)user_ctx;
    ctx->n_symbols = edata->num_symbols;
    BaseType_t hpw = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->waiter, &hpw);
    return hpw == pdTRUE;
}

int cc1101_rmt_rx(int16_t *out_pulses, int max_pulses,
                  uint32_t timeout_ms, uint32_t gap_us)
{
    if (!out_pulses || max_pulses <= 0) return 0;

    /* Each symbol yields up to 2 pulses. Size buffer accordingly. */
    int max_syms = (max_pulses + 1) / 2;
    rmt_symbol_word_t *syms = (rmt_symbol_word_t *)calloc(max_syms,
                                                          sizeof(rmt_symbol_word_t));
    if (!syms) return 0;

    rmt_channel_handle_t rx_chan = nullptr;
    int n_pulses = 0;
    rx_ctx_t ctx = { xTaskGetCurrentTaskHandle(), 0 };

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num          = (gpio_num_t)CC1101_GDO0;
    rx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz     = RMT_RESOLUTION_HZ;
    rx_cfg.mem_block_symbols = RMT_MEM_SYMBOLS;
    if (rmt_new_rx_channel(&rx_cfg, &rx_chan) != ESP_OK) goto done;

    {
        rmt_rx_event_callbacks_t cbs = {};
        cbs.on_recv_done = rx_done_cb;
        if (rmt_rx_register_event_callbacks(rx_chan, &cbs, &ctx) != ESP_OK) goto done;
    }

    if (rmt_enable(rx_chan) != ESP_OK) goto done;

    {
        rmt_receive_config_t rc = {};
        rc.signal_range_min_ns = 20 * 1000;           /* 20 µs glitch filter */
        rc.signal_range_max_ns = gap_us * 1000ULL;    /* gap_us silence = end */
        if (rmt_receive(rx_chan, syms, max_syms * sizeof(rmt_symbol_word_t),
                        &rc) != ESP_OK) goto done;
    }

    /* Wait for the rx_done notification or user timeout. */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        /* Timed out — no signal. Stop and clean up. */
        rmt_disable(rx_chan);
        goto done;
    }
    rmt_disable(rx_chan);

    /* Unpack symbols into pulses, skipping zero-duration tail padding. */
    for (int i = 0; i < ctx.n_symbols && n_pulses < max_pulses; ++i) {
        const rmt_symbol_word_t &s = syms[i];
        if (s.duration0 == 0) break;
        out_pulses[n_pulses++] = s.level0 ? (int16_t)s.duration0
                                          : -(int16_t)s.duration0;
        if (s.duration1 == 0) { /* last half-symbol or tail */ break; }
        if (n_pulses >= max_pulses) break;
        out_pulses[n_pulses++] = s.level1 ? (int16_t)s.duration1
                                          : -(int16_t)s.duration1;
    }

done:
    if (rx_chan) rmt_del_channel(rx_chan);
    free(syms);
    return n_pulses;
}
