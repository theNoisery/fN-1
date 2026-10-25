/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Browser reverb and delay. */
#include "mixer.h"
#include "fixed.h"
#include "synth/engine_fx.h"
#include <string.h>

static TapeDelayEngine global_delay;
static ReverbEngine global_reverb;
static int fx_is_init = 0;
static unsigned quiet_frames = UINT32_MAX;
static unsigned current_timeout = 0;
static int32_t global_level = 0;
static uint32_t target_dly_samples = 11025;

uint8_t custom_rev_type = 0;
uint8_t custom_rev_size = 85;
uint8_t custom_rev_decay = 60;
uint16_t custom_dly_fb_pct = 50;

static uint8_t last_rev_type = 255;
static uint8_t last_rev_size = 255;
static uint8_t last_rev_decay = 255;
static uint16_t last_dly_fb = 65535;
static uint32_t last_dly_samples = 0;

FM1_DSP_CODE void fm1_fx_set(int32_t decay, int32_t feedback, int32_t level, unsigned delay) {
    (void)decay;
    (void)feedback;

    if (!fx_is_init) {
        fx_delay_init(&global_delay);
        fx_reverb_init(&global_reverb);
        fx_is_init = 1;
    }

    global_level = level;
    // 'delay' is passed in 1/8th-rate ticks (both Free MS and Tempo Sync divisions).
    // Multiply by 8 to get the exact full-rate sample count!
    target_dly_samples = delay * 8U;
    current_timeout = delay > 1944U ? delay * 8U * 24U : FM1_SAMPLE_RATE * 8U;
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

static inline int32_t fm_add_sat32(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + b;
    if (sum > 8388607) return 8388607;
    if (sum < -8388608) return -8388608;
    return (int32_t)sum;
}

FM1_DSP_CODE void fm1_fx_render(int32_t *left_mix, int32_t *right_mix, int32_t *rev, int32_t *dly) {
    if (!fx_is_init) return;

    unsigned signal = 0;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        if (rev[i] > 167 || rev[i] < -167 || dly[i] > 167 || dly[i] < -167) { 
            signal = 1; break; 
        }
    }

    if (signal) {
        quiet_frames = 0;
    } else if (quiet_frames == UINT32_MAX) {
        return;
    } else {
        quiet_frames += FM1_BLOCK_FRAMES;
        if (quiet_frames >= current_timeout) { 
            fm1_fx_clear(); 
            return; 
        }
    }

    if (custom_rev_type != last_rev_type || custom_rev_size != last_rev_size || custom_rev_decay != last_rev_decay) {
        fx_reverb_set(&global_reverb, custom_rev_type, custom_rev_size, custom_rev_decay, 50, 100);
        last_rev_type = custom_rev_type;
        last_rev_size = custom_rev_size;
        last_rev_decay = custom_rev_decay;
    }

    if (custom_dly_fb_pct != last_dly_fb || target_dly_samples != last_dly_samples) {
        fx_delay_set(&global_delay, target_dly_samples, custom_dly_fb_pct, 100, 1);
        last_dly_fb = custom_dly_fb_pct;
        last_dly_samples = target_dly_samples;
    }

    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t dly_in = dly[i] / 512;
        int32_t rev_in = rev[i] / 512;

        int32_t dly_in_l = dly_in;
        int32_t dly_in_r = dly_in;
        int32_t dly_out_l = 0;
        int32_t dly_out_r = 0;
        fx_delay_process(&global_delay, &dly_in_l, &dly_in_r, &dly_out_l, &dly_out_r);

        // Only feed delay into reverb when the track's reverb send or delay is active
        int32_t rev_in_l = rev_in + (dly_out_l / 8);
        int32_t rev_in_r = rev_in + (dly_out_r / 8);
        int32_t rev_out_l = 0;
        int32_t rev_out_r = 0;
        fx_reverb_process(&global_reverb, &rev_in_l, &rev_in_r, &rev_out_l, &rev_out_r);

        int32_t out_l = (dly_out_l + rev_out_l) * 512;
        int32_t out_r = (dly_out_r + rev_out_r) * 512;
        
        out_l = fm_mul30(out_l, global_level);
        out_r = fm_mul30(out_r, global_level);

        left_mix[i] = fm_add_sat32(left_mix[i], out_l);
        right_mix[i] = fm_add_sat32(right_mix[i], out_r);
    }
}
