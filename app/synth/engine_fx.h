/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_FX_H
#define FM1_ENGINE_FX_H

#include <stdint.h>
#include <stddef.h>

enum {
    REV_TYPE_HALL = 0,
    REV_TYPE_PLATE,
    REV_TYPE_CHAMBER
};

// --- TAPE DELAY ---
#define DLY_MAX_SAMPLES 65536 // ~1.48 seconds at 44.1kHz

typedef struct {
    uint32_t target_time_l;
    uint32_t target_time_r;
    int32_t  current_time_l; // Q16 fixed-point for smooth gliding tape speed
    int32_t  current_time_r;
    
    uint16_t feedback;  // 0 to 150% (Scaled: 0 to 384)
    uint8_t  mix;       // 0 to 100
    uint8_t  ping_pong; // 0 or 1
    int8_t   lr_diff;   // -50% to +50% L/R time offset
    
    // Tape warmth low-pass & DC blocker states
    int32_t  lp_l, lp_r;
    int32_t  dc_in_l, dc_out_l;
    int32_t  dc_in_r, dc_out_r;

    int16_t  buf_l[DLY_MAX_SAMPLES];
    int16_t  buf_r[DLY_MAX_SAMPLES];
    uint32_t write_idx;
} TapeDelayEngine;

// --- MULTI-ALGO REVERB (FDN) ---
#define FDN_LINES 4
#define FDN_MAX_LEN 4096
#define AP_MAX_LEN 1024

typedef struct {
    uint8_t  type;     // REV_TYPE_HALL, PLATE, CHAMBER
    uint8_t  size;     // 0 to 100
    uint8_t  decay;    // 0 to 100 (Feedback)
    uint8_t  mix;      // 0 to 100
    uint8_t  er_level; // Early Reflections level
    
    uint16_t fdn_lengths[FDN_LINES];
    uint16_t ap_lengths[FDN_LINES];
    
    int32_t  fdn_lp[FDN_LINES];
    int32_t  in_lp_l, in_lp_r;

    int16_t  fdn_buf[FDN_LINES][FDN_MAX_LEN];
    int16_t  ap_buf[FDN_LINES][AP_MAX_LEN];
    
    uint32_t fdn_idx[FDN_LINES];
    uint32_t ap_idx[FDN_LINES];
    
    uint32_t lfo_phase;
} ReverbEngine;

void fx_delay_init(TapeDelayEngine *dly);
void fx_delay_set(TapeDelayEngine *dly, uint32_t delay_samples, uint16_t feedback_pct, uint8_t mix, uint8_t ping_pong, int8_t lr_diff);
void fx_delay_process(TapeDelayEngine *dly, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r);

void fx_reverb_init(ReverbEngine *rev);
void fx_reverb_set(ReverbEngine *rev, uint8_t type, uint8_t size, uint8_t decay, uint8_t er_level, uint8_t mix);
void fx_reverb_process(ReverbEngine *rev, int32_t *in_l, int32_t *in_r, int32_t *out_l, int32_t *out_r);

#endif
