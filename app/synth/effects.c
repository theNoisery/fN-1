/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine.h"
#include "engine_fx.h"
#include <string.h>

static TapeDelayEngine global_delay;
static ReverbEngine global_reverb;
static int fx_is_init = 0;
static unsigned quiet_frames = UINT32_MAX;
static unsigned current_timeout = 0;

// FM1 uses Q30 fixed point for its 0.0 - 1.0 parameter ranges (1.0 = 1073741824)
FM1_DSP_CODE void fm1_fx_set(int32_t decay, int32_t feedback, int32_t level, unsigned delay) {
    if (!fx_is_init) {
        fx_delay_init(&global_delay);
        fx_reverb_init(&global_reverb);
        fx_is_init = 1;
    }

    // Map the hardware's original Q30 parameters to our 0-100% ranges
    uint8_t rev_time = (uint8_t)((decay * 100LL) >> 30);
    uint16_t dly_fb = (uint16_t)((feedback * 150LL) >> 30); // 150% self-oscillation max!
    uint8_t fx_mix = (uint8_t)((level * 100LL) >> 30);
    
    // delay is passed in raw samples at 32kHz
    uint32_t dly_ms = (delay * 1000U) / 32000U;

    // Hardcode a massive Hall Reverb for now (Type 0, Size 85, ER 50)
    // We can expose these to the global FX UI later!
    fx_reverb_set(&global_reverb, 0, 85, rev_time, 50, fx_mix);
    fx_delay_set(&global_delay, dly_ms, dly_fb, fx_mix, 1); // 1 = Stereo Ping-Pong

    current_timeout = delay > 1944U ? delay * 8U * 24U : 32000U * 8U;
}

FM1_DSP_CODE void fm1_fx_clear(void) {
    if (quiet_frames == UINT32_MAX) return;
    fx_delay_init(&global_delay);
    fx_reverb_init(&global_reverb);
    quiet_frames = UINT32_MAX;
}

FM1_DSP_CODE unsigned fm1_fx_active(void) {
    return quiet_frames != UINT32_MAX;
}

// Saturation helper to prevent the master bus from clipping when feedback > 100%
static inline int32_t fm_add_sat32(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + b;
    if (sum > 8388607) return 8388607;
    if (sum < -8388608) return -8388608;
    return (int32_t)sum;
}

// This overrides the hidden binary function and receives the per-track sends!
FM1_DSP_CODE void fm1_fx_render(int32_t *left_mix, int32_t *right_mix, int32_t *rev, int32_t *dly) {
    if (!fx_is_init) return;

    unsigned signal = 0;
    for (unsigned i = 0; i < 32; ++i) { // Standard 32-sample block
        if (rev[i] > 167 || rev[i] < -167 || dly[i] > 167 || dly[i] < -167) { 
            signal = 1; break; 
        }
    }

    if (signal) {
        quiet_frames = 0;
    } else if (quiet_frames == UINT32_MAX) {
        return;
    } else {
        quiet_frames += 32;
        if (quiet_frames >= current_timeout) { 
            fm1_fx_clear(); 
            return; 
        }
    }

    for (unsigned i = 0; i < 32; ++i) {
        // FM1 sends audio in highly scaled integer format.
        // Divide by 256 to fit inside our 16-bit DSP engine headroom without clipping.
        int32_t dly_in = dly[i] / 256;
        int32_t rev_in = rev[i] / 256;

        int32_t dly_in_l = dly_in;
        int32_t dly_in_r = dly_in;
        int32_t dly_out_l = 0;
        int32_t dly_out_r = 0;
        fx_delay_process(&global_delay, &dly_in_l, &dly_in_r, &dly_out_l, &dly_out_r);

        // Cascade the delay tail into the reverb to glue the space together!
        int32_t rev_in_l = rev_in + (dly_out_l / 4);
        int32_t rev_in_r = rev_in + (dly_out_r / 4);
        int32_t rev_out_l = 0;
        int32_t rev_out_r = 0;
        fx_reverb_process(&global_reverb, &rev_in_l, &rev_in_r, &rev_out_l, &rev_out_r);

        // Scale back up to 24-bit headroom and mix into the master bus
        left_mix[i] = fm_add_sat32(left_mix[i], (dly_out_l + rev_out_l) * 256);
        right_mix[i] = fm_add_sat32(right_mix[i], (dly_out_r + rev_out_r) * 256);
    }
}
