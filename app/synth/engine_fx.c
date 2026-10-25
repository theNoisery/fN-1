/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "synth/engine_fx.h"
#include <string.h>

// ---------------------------------------------------------
// CORE DSP UTILITIES
// ---------------------------------------------------------

static inline int16_t tape_saturate(int32_t x) {
    if (x > 16384) {
        int32_t over = x - 16384;
        return (int16_t)(16384 + ((over * 16383) / (over + 8192)));
    } else if (x < -16384) {
        int32_t over = -x - 16384;
        return (int16_t)(-16384 - ((over * 16383) / (over + 8192)));
    }
    return (int16_t)x;
}

// ---------------------------------------------------------
// TAPE DELAY (>100% FEEDBACK & EXACT TEMPO SYNC)
// ---------------------------------------------------------

void fx_delay_init(TapeDelayEngine *dly) {
    memset(dly, 0, sizeof(TapeDelayEngine));
    dly->time_l = 11025; // Default 250ms at 44.1kHz
    dly->time_r = 11025;
}

// Note: 'delay_samples' is now passed as exact native audio frames (delay_ticks * 8)
void fx_delay_set(TapeDelayEngine *dly, uint32_t delay_samples, uint16_t feedback_pct, uint8_t mix, uint8_t ping_pong) {
    uint32_t samps = delay_samples;
    if (samps < 64U) samps = 64U;
    if (samps >= DLY_MAX_SAMPLES) samps = DLY_MAX_SAMPLES - 1U;
    
    // Both L and R use the exact synced sample length so repeats stay locked to BPM
    dly->time_l = samps;
    dly->time_r = samps;
    dly->feedback = (feedback_pct * 256U) / 100U;
    dly->mix = mix;
    dly->ping_pong = ping_pong;
}

void fx_delay_process(TapeDelayEngine *dly, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r) {
    uint32_t read_l = (dly->write_idx + DLY_MAX_SAMPLES - dly->time_l) & (DLY_MAX_SAMPLES - 1);
    uint32_t read_r = (dly->write_idx + DLY_MAX_SAMPLES - dly->time_r) & (DLY_MAX_SAMPLES - 1);

    int32_t dly_out_l = dly->buf_l[read_l];
    int32_t dly_out_r = dly->buf_r[read_r];

    int32_t sum_l, sum_r;
    if (dly->ping_pong) {
        // True tempo-synced Ping-Pong: Input enters L, L feeds R, R feeds back into L
        int32_t mono_in = (*in_l + *in_r) >> 1;
        sum_l = mono_in + ((dly_out_r * dly->feedback) >> 8);
        sum_r = ((dly_out_l * dly->feedback) >> 8);
    } else {
        sum_l = *in_l + ((dly_out_l * dly->feedback) >> 8);
        sum_r = *in_r + ((dly_out_r * dly->feedback) >> 8);
    }

    dly->buf_l[dly->write_idx] = tape_saturate(sum_l);
    dly->buf_r[dly->write_idx] = tape_saturate(sum_r);
    
    dly->write_idx = (dly->write_idx + 1) & (DLY_MAX_SAMPLES - 1);

    // Return WET ONLY for the master send bus
    *out_l = ((dly_out_l * dly->mix) / 100);
    *out_r = ((dly_out_r * dly->mix) / 100);
}

// ---------------------------------------------------------
// MULTI-ALGO FDN REVERB
// ---------------------------------------------------------

static const uint16_t rev_primes[3][FDN_LINES] = {
    { 1423, 1789, 2131, 2459 }, // HALL
    { 401,  503,  613,  727  }, // PLATE
    { 787,  883,  1013, 1153 }  // CHAMBER
};

static const uint16_t ap_primes[3][FDN_LINES] = {
    { 223, 263, 311, 359 }, // HALL
    { 541, 659, 761, 887 }, // PLATE
    { 151, 191, 239, 281 }  // CHAMBER
};

void fx_reverb_init(ReverbEngine *rev) {
    memset(rev, 0, sizeof(ReverbEngine));
    fx_reverb_set(rev, REV_TYPE_HALL, 50, 70, 50, 30);
}

void fx_reverb_set(ReverbEngine *rev, uint8_t type, uint8_t size, uint8_t decay, uint8_t er_level, uint8_t mix) {
    if (type > 2) type = 0;
    rev->type = type;
    rev->size = size;
    rev->decay = decay;
    rev->mix = mix;
    rev->er_level = er_level;

    uint32_t scale = 50 + (size * 150) / 100;

    for (int i = 0; i < FDN_LINES; i++) {
        rev->fdn_lengths[i] = (rev_primes[type][i] * scale) / 100;
        if (rev->fdn_lengths[i] >= FDN_MAX_LEN) rev->fdn_lengths[i] = FDN_MAX_LEN - 1;

        rev->ap_lengths[i] = ap_primes[type][i];
    }
}

void fx_reverb_process(ReverbEngine *rev, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r) {
    int32_t input = (*in_l + *in_r) >> 1;
    
    int32_t ap_coeff = (rev->type == REV_TYPE_PLATE) ? 20480 : 16384; 
    
    int32_t diffused = input;
    for (int i = 0; i < FDN_LINES; i++) {
        int16_t ap_read = rev->ap_buf[i][rev->ap_idx[i]];
        int32_t ap_out = ap_read - ((diffused * ap_coeff) >> 15);
        rev->ap_buf[i][rev->ap_idx[i]] = tape_saturate(diffused + ((ap_out * ap_coeff) >> 15));
        
        diffused = ap_out;
        if (++rev->ap_idx[i] >= rev->ap_lengths[i]) rev->ap_idx[i] = 0;
    }

    int32_t er_tap = rev->fdn_buf[0][(rev->fdn_idx[0] + FDN_MAX_LEN - (rev->fdn_lengths[0] / 3)) & (FDN_MAX_LEN - 1)];

    int32_t fdn_out[FDN_LINES];
    for (int i = 0; i < FDN_LINES; i++) {
        fdn_out[i] = rev->fdn_buf[i][rev->fdn_idx[i]];
    }

    int32_t fb_gain = (rev->decay * 32112) / 100;
    int32_t in_matrix[FDN_LINES];

    in_matrix[0] =  fdn_out[0] + fdn_out[1] + fdn_out[2] + fdn_out[3];
    in_matrix[1] =  fdn_out[0] - fdn_out[1] + fdn_out[2] - fdn_out[3];
    in_matrix[2] =  fdn_out[0] + fdn_out[1] - fdn_out[2] - fdn_out[3];
    in_matrix[3] =  fdn_out[0] - fdn_out[1] - fdn_out[2] + fdn_out[3];

    for (int i = 0; i < FDN_LINES; i++) {
        int32_t fb_signal = (in_matrix[i] * fb_gain) >> 16; 
        rev->fdn_buf[i][rev->fdn_idx[i]] = tape_saturate(diffused + fb_signal);
        if (++rev->fdn_idx[i] >= rev->fdn_lengths[i]) rev->fdn_idx[i] = 0;
    }

    int32_t wet_l = fdn_out[0] - fdn_out[2] + ((er_tap * rev->er_level) >> 7);
    int32_t wet_r = fdn_out[1] + fdn_out[3] + ((er_tap * rev->er_level) >> 7);

    // Return WET ONLY for the master send bus
    *out_l = ((wet_l * rev->mix) / 100);
    *out_r = ((wet_r * rev->mix) / 100);
}
