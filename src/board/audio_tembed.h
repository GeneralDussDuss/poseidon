/*
 * audio_tembed - configures M5Unified's Speaker_Class for the T-Embed's
 * NS4168 I2S speaker (BCLK 46, WS 40, DOUT 7). main.cpp's TEMBED init
 * sets cfg.internal_spk = false before M5.begin(), so nothing drives the
 * speaker until this runs.
 */
#pragma once

#if defined(POSEIDON_BOARD_TEMBED)

/* Configures M5.Speaker for the NS4168 and calls M5.Speaker.begin().
 * src/sfx.cpp calls into M5Cardputer.Speaker, which is a reference to
 * this same M5.Speaker object (see M5Cardputer.h), so no change to
 * sfx.cpp is needed — existing sfx_* calls start producing sound once
 * this has run. */
void tembed_speaker_init(void);

#endif /* POSEIDON_BOARD_TEMBED */
