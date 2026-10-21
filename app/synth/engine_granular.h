/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_GRANULAR_H
#define FM1_ENGINE_GRANULAR_H

#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 8

typedef struct {
    int32_t position;    // Read pointer (Q16.16)
    int32_t speed;       // Pitch multiplier (Q16.16)
    int16_t length;      // Grain length in samples
    int16_t counter;     // Current position inside grain
    uint8_t active;      // Active flag
} Grain;

typedef struct {
    uint32_t sample_rate;
    const int16_t *sample_buffer; // Pointer to shared sample slot
    uint32_t sample_length;
    
    // OP-1 Macro Knobs (K1 - K4)
    uint16_t macro_scan;     // K1: Scan position
    uint16_t macro_size;     // K2: Grain size
    uint16_t macro_density;  // K3: Density / spawn rate
    uint16_t macro_spray;    // K4: Pitch scatter
    
    uint32_t spawn_counter;
    Grain grains[MAX_GRAINS];
} GranularVoiceState;

void granular_init(void *state, uint32_t sample_rate);
void granular_note_on(void *state, uint8_t note, uint8_t vel);
void granular_note_off(void *state, uint8_t note);
void granular_set_macro(void *state, uint8_t knob_idx, int16_t val);
void granular_render_block(void *state, int16_t *out_l, int16_t *out_r, size_t frames);

#endif
