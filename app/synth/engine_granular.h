/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_GRANULAR_H
#define FM1_ENGINE_GRANULAR_H

#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 24

enum {
    GRAN_ENV_IDLE = 0,
    GRAN_ENV_ATTACK,
    GRAN_ENV_DECAY,
    GRAN_ENV_SUSTAIN,
    GRAN_ENV_RELEASE
};

enum {
    GRAN_FILT_OFF = 0,
    GRAN_FILT_LPF,
    GRAN_FILT_BPF,
    GRAN_FILT_HPF
};

typedef struct {
    uint32_t position;
    uint32_t speed;
    uint32_t length;
    uint32_t counter;
    uint8_t pan;
    uint8_t active;
} Grain;

typedef struct {
    uint32_t sample_rate;
    const int16_t *sample_buffer;
    uint32_t sample_length;

    // Calibrated Macros: Scan (1-100%), Size (10-500ms), Density (1-24), Spray (0-100%)
    uint16_t macro_scan;
    uint16_t macro_size;
    uint16_t macro_density;
    uint16_t macro_spray;

    // Master ADSR Envelope: Attack/Decay/Release in ms (0-2000), Sustain in dB (-60 to 0)
    uint16_t env_attack;
    uint16_t env_decay;
    int16_t  env_sustain;
    uint16_t env_release;
    uint8_t  env_stage;
    uint32_t env_level; // Q16: 0 to 65535

    // Stereo Multimode Filter + Chorus (Edit Page 2: K1-K4)
    uint8_t filter_mode;   // 0=OFF, 1=LPF, 2=BPF, 3=HPF
    uint8_t filter_cutoff; // 0-127
    uint8_t filter_res;    // 0-127
    uint8_t chorus_depth;  // 0-100%
    int32_t svf_low_l, svf_band_l;
    int32_t svf_low_r, svf_band_r;

    // Track volume (0-100) and Live FX Bus settings synced from the FX screen
    uint8_t volume;
    uint8_t rev_send;
    uint8_t dly_send;
    uint8_t rev_time;
    uint8_t dly_feedback;
    uint8_t fx_return;
    uint32_t dly_samples;

    uint32_t current_speed;
    uint32_t spawn_counter;
    uint32_t next_spawn_delay;
    uint32_t rng_state;
    uint8_t gate;
    Grain grains[MAX_GRAINS];
} GranularVoiceState;

void granular_init(void *state, uint32_t sample_rate);
void granular_note_on(void *state, uint8_t note, uint8_t vel);
void granular_note_off(void *state, uint8_t note);
void granular_set_macro(void *state, uint8_t knob_idx, int16_t val);
void granular_set_env(void *state, uint8_t knob_idx, int16_t val);
void granular_set_filter(void *state, uint8_t knob_idx, int16_t val);
void granular_set_fx(void *state, uint8_t rev_send, uint8_t dly_send,
                     uint8_t rev_time, uint8_t dly_fb, uint8_t fx_ret, uint32_t dly_ticks);
void granular_render_block(void *state, int16_t *out_l, int16_t *out_r, size_t frames);
void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length);
void granular_set_active_slot(uint8_t slot);
uint8_t granular_get_active_slot(void);

#endif
