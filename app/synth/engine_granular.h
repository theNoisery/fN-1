/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_GRANULAR_H
#define FM1_ENGINE_GRANULAR_H

#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 48
#define GRAN_MAX_VOICES 6

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
    uint16_t start_delay;
    uint8_t pan;
    uint8_t voice_idx;
    uint8_t active;
} Grain;

typedef struct {
    uint8_t active;
    uint8_t gate;
    uint8_t note;
    uint8_t env_stage;
    uint32_t env_level; // Q16: 0 to 65535
    uint32_t current_speed;
    uint32_t spawn_counter;
    uint32_t next_spawn_delay;
    uint32_t age;
} GranularPolyVoice;

typedef struct {
    uint32_t sample_rate;
    const int16_t *sample_buffer;
    uint32_t sample_length;

    // Page 1 Macros: Scan (1-100%), Size (10-500ms), Density (1-24), Spray (0-100%)
    uint16_t macro_scan;
    uint16_t macro_size;
    uint16_t macro_density;
    uint16_t macro_spray;

    // Page 2 Space/Pitch/Poly: Stereo L/R Offset (0-100%), Pitch Rand (0-100% = 2 oct), Voices (1-6), Chorus (0-100%)
    uint16_t macro_stereo;
    uint16_t macro_pitch_rnd;
    uint8_t  poly_voices;
    uint8_t  chorus_depth;

    // Master ADSR Envelope: Attack/Decay/Release in ms (0-2000), Sustain in dB (-60 to 0)
    uint16_t env_attack;
    uint16_t env_decay;
    int16_t  env_sustain;
    uint16_t env_release;

    // Page 3 Stereo Multimode Filter: Mode, Cutoff, Resonance (Q), Slope (12dB/24dB/36dB)
    uint8_t filter_mode;   // 0=OFF, 1=LPF, 2=BPF, 3=HPF
    uint8_t filter_cutoff; // 0-127
    uint8_t filter_res;    // 0-127
    uint8_t filter_slope;  // 0=12dB, 1=24dB, 2=36dB
    int32_t svf_low_l[3], svf_band_l[3];
    int32_t svf_low_r[3], svf_band_r[3];

    // Track volume (0-100)
    uint8_t volume;

    uint32_t voice_age_counter;
    uint32_t rng_state;
    GranularPolyVoice voices[GRAN_MAX_VOICES];
    Grain grains[MAX_GRAINS];
} GranularVoiceState;

void granular_init(void *state, uint32_t sample_rate);
void granular_note_on(void *state, uint8_t note, uint8_t vel);
void granular_note_off(void *state, uint8_t note);
void granular_set_macro(void *state, uint8_t knob_idx, int16_t val);
void granular_set_space(void *state, uint8_t knob_idx, int16_t val);
void granular_set_env(void *state, uint8_t knob_idx, int16_t val);
void granular_set_filter(void *state, uint8_t knob_idx, int16_t val);
void granular_render_block(void *state, int16_t *out_l, int16_t *out_r, size_t frames);
void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length);
void granular_set_active_slot(uint8_t slot);
uint8_t granular_get_active_slot(void);

#endif
