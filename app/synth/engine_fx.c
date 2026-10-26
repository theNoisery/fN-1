/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "synth/engine_fx.h"
#include <string.h>

// Define these globally for the hardware firmware build
#ifndef __wasm__
int32_t custom_dly_lr_diff = 0;
int32_t custom_rev_type = 0;
int32_t custom_rev_decay = 50;
int32_t custom_rev_size = 50;
int32_t custom_rev_er = 30;
int32_t custom_dly_fb_pct = 50;
int32_t custom_dly_time = 250;
int32_t custom_rev_mix = 30;
int32_t custom_dly_mix = 30;
#endif

// ---------------------------------------------------------
// CORE DSP UTILITIES
// ---------------------------------------------------------

// Ultra-smooth rational tanh soft-clipper without a hard knee.
// Completely linear for quiet/normal signals, gently compresses loud peaks
// into warm saturation when feedback > 100%.
static inline int16_t tape_saturate(int32_t x) {
    if (x > 98304) return 32767;
    if (x < -98304) return -32767;
    // Normalize to Q15 (-3.0 to +3.0 range) and apply smooth x * (27 + x^2) / (27 + 9*x^2) curve
    int64_t q = x;
    int64_t x2 = (q * q) >> 15;
    int64_t num = q * (884736LL + x2);          // 27 * 32768 = 884736
    int64_t den = 884736LL + (x2 * 3);          // Tuned ceiling to approach +/- 32767 smoothly
    int32_t out = (int32_t)(num / den);
    if (out > 32767) out = 32767;
    if (out < -32767) out = -32767;
    return (int16_t)out;
}

// Simple hard clamp for clean internal buffers that already have headroom
static inline int16_t clean_clamp16(int32_t x) {
    if (x > 32767) return 32767;
    if (x < -32767) return -32767;
    return (int16_t)x;
}

// ---------------------------------------------------------
// TAPE DELAY (VARISPEED PITCH-SHIFTING TIME SWEEPS)
// ---------------------------------------------------------

void fx_delay_init(TapeDelayEngine *dly) {
    memset(dly, 0, sizeof(TapeDelayEngine));
    dly->target_time_l = 11025;
    dly->target_time_r = 11025;
    dly->current_time_l = 11025 << 16;
    dly->current_time_r = 11025 << 16;
}

void fx_delay_set(TapeDelayEngine *dly, uint32_t delay_samples, uint16_t feedback_pct, uint8_t mix, uint8_t ping_pong, int8_t lr_diff) {
    uint32_t samps_l = delay_samples;
    if (samps_l < 64U) samps_l = 64U;
    if (samps_l >= DLY_MAX_SAMPLES) samps_l = DLY_MAX_SAMPLES - 1U;

    int32_t diff_clamped = lr_diff < -50 ? -50 : lr_diff > 50 ? 50 : lr_diff;
    uint32_t samps_r = (uint32_t)(((int64_t)samps_l * (100 + diff_clamped)) / 100);
    if (samps_r < 64U) samps_r = 64U;
    if (samps_r >= DLY_MAX_SAMPLES) samps_r = DLY_MAX_SAMPLES - 1U;

    dly->target_time_l = samps_l;
    dly->target_time_r = samps_r;
    dly->feedback = (feedback_pct * 256U) / 100U;
    dly->mix = mix;
    dly->ping_pong = ping_pong;
    dly->lr_diff = (int8_t)diff_clamped;
}

void fx_delay_process(TapeDelayEngine *dly, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r) {
    // Smooth glide for delay time changes (creates the varispeed tape pitch-bend effect!)
    dly->current_time_l += ((int32_t)((dly->target_time_l << 16) - dly->current_time_l)) >> 9;
    dly->current_time_r += ((int32_t)((dly->target_time_r << 16) - dly->current_time_r)) >> 9;

    uint32_t time_l_q16 = (uint32_t)dly->current_time_l;
    uint32_t time_r_q16 = (uint32_t)dly->current_time_r;

    // Calculate fractional read positions in the circular buffer
    uint32_t base_idx_l = (dly->write_idx + DLY_MAX_SAMPLES - (time_l_q16 >> 16)) & (DLY_MAX_SAMPLES - 1);
    uint32_t frac_l = time_l_q16 & 0xFFFFU;
    uint32_t prev_idx_l = (base_idx_l + DLY_MAX_SAMPLES - 1) & (DLY_MAX_SAMPLES - 1);

    uint32_t base_idx_r = (dly->write_idx + DLY_MAX_SAMPLES - (time_r_q16 >> 16)) & (DLY_MAX_SAMPLES - 1);
    uint32_t frac_r = time_r_q16 & 0xFFFFU;
    uint32_t prev_idx_r = (base_idx_r + DLY_MAX_SAMPLES - 1) & (DLY_MAX_SAMPLES - 1);

    // Linear interpolation between adjacent samples for smooth pitch gliding
    int32_t s0_l = dly->buf_l[base_idx_l];
    int32_t s1_l = dly->buf_l[prev_idx_l];
    int32_t dly_out_l = s0_l + (((s1_l - s0_l) * (int32_t)frac_l) >> 16);

    int32_t s0_r = dly->buf_r[base_idx_r];
    int32_t s1_r = dly->buf_r[prev_idx_r];
    int32_t dly_out_r = s0_r + (((s1_r - s0_r) * (int32_t)frac_r) >> 16);

    // Warm tape low-pass filter
    dly->lp_l += ((dly_out_l - dly->lp_l) * 140) >> 8;
    dly->lp_r += ((dly_out_r - dly->lp_r) * 140) >> 8;

    int32_t filt_l = dly->lp_l;
    int32_t filt_r = dly->lp_r;

    int32_t in_scaled_l = *in_l >> 1;
    int32_t in_scaled_r = *in_r >> 1;

    int32_t sum_l, sum_r;
    if (dly->ping_pong) {
        int32_t mono_in = (in_scaled_l + in_scaled_r) >> 1;
        sum_l = mono_in + ((filt_r * dly->feedback) >> 8);
        sum_r = (dly->lr_diff != 0 ? (mono_in >> 1) : 0) + ((filt_l * dly->feedback) >> 8);
    } else {
        sum_l = in_scaled_l + ((filt_l * dly->feedback) >> 8);
        sum_r = in_scaled_r + ((filt_r * dly->feedback) >> 8);
    }

    // DC Blocker
    int32_t dc_l = sum_l - dly->dc_in_l + ((dly->dc_out_l * 253) >> 8);
    dly->dc_in_l = sum_l;
    dly->dc_out_l = dc_l;

    int32_t dc_r = sum_r - dly->dc_in_r + ((dly->dc_out_r * 253) >> 8);
    dly->dc_in_r = sum_r;
    dly->dc_out_r = dc_r;

    dly->buf_l[dly->write_idx] = tape_saturate(dc_l);
    dly->buf_r[dly->write_idx] = tape_saturate(dc_r);
    
    dly->write_idx = (dly->write_idx + 1) & (DLY_MAX_SAMPLES - 1);

    *out_l = ((filt_l * 2 * dly->mix) / 100);
    *out_r = ((filt_r * 2 * dly->mix) / 100);
}

// ---------------------------------------------------------
// MULTI-ALGO FDN REVERB (HIGH-DENSITY DIFFUSION & CLEAN)
// ---------------------------------------------------------

static const uint16_t rev_primes[3][FDN_LINES] = {
    { 1423, 1789, 2131, 2459 }, // HALL: Wide, expansive prime lengths
    { 503,  613,  727,  853  }, // PLATE: Tight, dense build-up
    { 787,  941,  1103, 1277 }  // CHAMBER: Medium acoustic room
};

static const uint16_t ap_primes[3][FDN_LINES] = {
    { 149, 223, 311, 439 }, // HALL
    { 193, 283, 397, 541 }, // PLATE
    { 113, 173, 251, 347 }  // CHAMBER
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

        rev->ap_lengths[i] = (ap_primes[type][i] * (75 + (size >> 1))) / 100;
        if (rev->ap_lengths[i] >= AP_MAX_LEN) rev->ap_lengths[i] = AP_MAX_LEN - 1;
        if (rev->ap_lengths[i] < 32) rev->ap_lengths[i] = 32;
    }
}

void fx_reverb_process(ReverbEngine *rev, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r) {
    // 1. Gently smooth harsh input transients without throwing away bit-depth
    rev->in_lp_l += ((*in_l - rev->in_lp_l) * 160) >> 8;
    rev->in_lp_r += ((*in_r - rev->in_lp_r) * 160) >> 8;

    int32_t mono_in = (rev->in_lp_l + rev->in_lp_r) >> 1;
    int32_t stereo_diff = (rev->in_lp_l - rev->in_lp_r) >> 2;

    // 2. 4-Stage Cascaded Series Allpass Diffuser (2^4 = 16x transient smearing)
    // High allpass gain (0.65 for Plate, 0.60 for Hall/Chamber) turns sharp clicks into a soft cloud
    int32_t ap_coeff = (rev->type == REV_TYPE_PLATE) ? 21299 : 19660;
    int32_t diffused = mono_in >> 1; // -6dB headroom for the series chain

    for (int i = 0; i < FDN_LINES; i++) {
        uint32_t idx = rev->ap_idx[i];
        if (idx >= rev->ap_lengths[i]) idx = 0;

        int32_t delayed = rev->ap_buf[i][idx];
        int32_t v = diffused - ((delayed * ap_coeff) >> 15);
        rev->ap_buf[i][idx] = tape_saturate(v);
        diffused = delayed + ((v * ap_coeff) >> 15);

        if (++idx >= rev->ap_lengths[i]) idx = 0;
        rev->ap_idx[i] = idx;
    }

    // Multi-tap Early Reflections from across the FDN delay lines
    int32_t er_l = rev->fdn_buf[0][(rev->fdn_idx[0] + (rev->fdn_lengths[0] >> 2)) % rev->fdn_lengths[0]]
                 + rev->fdn_buf[2][(rev->fdn_idx[2] + (rev->fdn_lengths[2] >> 1)) % rev->fdn_lengths[2]];
    int32_t er_r = rev->fdn_buf[1][(rev->fdn_idx[1] + (rev->fdn_lengths[1] >> 2)) % rev->fdn_lengths[1]]
                 + rev->fdn_buf[3][(rev->fdn_idx[3] + (rev->fdn_lengths[3] >> 1)) % rev->fdn_lengths[3]];

    // 3. Read FDN lines and apply warm acoustic HF damping per line
    int32_t damp = (rev->type == REV_TYPE_HALL) ? 110 : (rev->type == REV_TYPE_CHAMBER) ? 145 : 180;
    int32_t fdn_out[FDN_LINES];
    for (int i = 0; i < FDN_LINES; i++) {
        if (rev->fdn_idx[i] >= rev->fdn_lengths[i]) rev->fdn_idx[i] = 0;
        int32_t raw = rev->fdn_buf[i][rev->fdn_idx[i]];
        rev->fdn_lp[i] += ((raw - rev->fdn_lp[i]) * damp) >> 8;
        fdn_out[i] = rev->fdn_lp[i];
    }

    // 4. Orthogonal Householder Matrix (0.5x scaling = >> 1)
    int32_t sum_all = (fdn_out[0] + fdn_out[1] + fdn_out[2] + fdn_out[3]) >> 1;
    int32_t fb_gain = 16384 + (rev->decay * 15500) / 100; // 0.50 to 0.973 decay

    for (int i = 0; i < FDN_LINES; i++) {
        int32_t matrix_out = sum_all - fdn_out[i];
        int32_t fb_signal = (matrix_out * fb_gain) >> 15;

        // Inject diffused signal + stereo spread with alternating phase so DC never builds up
        int32_t spread = (i & 1) ? -stereo_diff : stereo_diff;
        int32_t tank_in = ((i & 2) ? -diffused : diffused) + spread;

        rev->fdn_buf[i][rev->fdn_idx[i]] = tape_saturate(tank_in + fb_signal);
        if (++rev->fdn_idx[i] >= rev->fdn_lengths[i]) rev->fdn_idx[i] = 0;
    }

    // 5. Decorrelated Stereo Extraction
    int32_t wet_l = (fdn_out[0] - fdn_out[1] + fdn_out[2] - fdn_out[3]) + ((er_l * rev->er_level) >> 7);
    int32_t wet_r = (fdn_out[0] + fdn_out[1] - fdn_out[2] - fdn_out[3]) + ((er_r * rev->er_level) >> 7);

    *out_l = ((wet_l * rev->mix) / 100);
    *out_r = ((wet_r * rev->mix) / 100);
}
