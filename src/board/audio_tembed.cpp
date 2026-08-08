#if defined(POSEIDON_BOARD_TEMBED)

#include "audio_tembed.h"

#include <Arduino.h>
#include <M5Unified.h>

#include "board_tembed.h"

/* Field names verified against
 * .pio/libdeps/tembed/M5Unified/src/utility/Speaker_Class.hpp's
 * speaker_config_t (pin_data_out, pin_bck, pin_ws, use_dac, i2s_port —
 * not guessed). */
/* One cycle of a sine, 8-bit unsigned. Passed to Speaker::tone() as the
 * waveform so notes are smooth instead of the default square wave. A square
 * is all odd harmonics, which on a small class-D amp is the harsh buzzy
 * "gritty" character; a sine removes it entirely. */
const uint8_t TEMBED_SINE32[32] = {
    128, 152, 176, 198, 218, 234, 245, 253,
    255, 253, 245, 234, 218, 198, 176, 152,
    128, 103,  79,  57,  37,  21,  10,   2,
      0,   2,  10,  21,  37,  57,  79, 103,
};

void tembed_speaker_init(void)
{
    auto cfg = M5.Speaker.config();
    cfg.pin_bck      = TE_I2S_BCLK;
    cfg.pin_ws        = TE_I2S_WS;
    cfg.pin_data_out  = TE_I2S_DOUT;
    cfg.i2s_port      = I2S_NUM_0;
    cfg.use_dac       = false;

    /* Static and crackle on this board were DMA underruns, not the amp: the
     * UI loop starves the audio task and the default buffers are too small
     * to ride it out. Bigger buffers plus a higher-priority pinned task fix
     * it. 48 kHz also removes the aliasing that made short blips sound like
     * noise bursts. */
    cfg.sample_rate       = 48000;
    cfg.dma_buf_len       = 256;
    cfg.dma_buf_count     = 8;
    cfg.task_priority     = 2;
    cfg.task_pinned_core  = 1;   /* keep audio off the core running the UI */
    cfg.magnification     = 12;  /* below the default to leave clipping headroom */

    M5.Speaker.config(cfg);
    M5.Speaker.begin();
}

void tembed_chirp(float f0, float f1, uint32_t ms)
{
    /* Short rising/falling glide built from a handful of sine segments.
     * Reads as a chirp rather than a step because each segment is only a
     * few ms and the waveform underneath is smooth. */
    const int steps = 8;
    if (ms < (uint32_t)steps) { ms = steps; }
    const uint32_t seg = ms / steps;
    for (int i = 0; i < steps; i++) {
        float f = f0 + (f1 - f0) * ((float)i / (float)(steps - 1));
        M5.Speaker.tone(f, seg, 0, (i == 0), TEMBED_SINE32, sizeof(TEMBED_SINE32));
        delay(seg);
    }
}

#endif /* POSEIDON_BOARD_TEMBED */
