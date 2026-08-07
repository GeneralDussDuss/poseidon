#if defined(POSEIDON_BOARD_TEMBED)

#include "audio_tembed.h"

#include <Arduino.h>
#include <M5Unified.h>

#include "board_tembed.h"

/* Field names verified against
 * .pio/libdeps/tembed/M5Unified/src/utility/Speaker_Class.hpp's
 * speaker_config_t (pin_data_out, pin_bck, pin_ws, use_dac, i2s_port —
 * not guessed). */
void tembed_speaker_init(void)
{
    auto cfg = M5.Speaker.config();
    cfg.pin_bck      = TE_I2S_BCLK;
    cfg.pin_ws        = TE_I2S_WS;
    cfg.pin_data_out  = TE_I2S_DOUT;
    cfg.i2s_port      = I2S_NUM_0;
    cfg.use_dac       = false;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
}

#endif /* POSEIDON_BOARD_TEMBED */
