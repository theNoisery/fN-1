/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_GRANULAR_H
#define FM1_ENGINE_GRANULAR_H

#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 200

typedef struct {
    int32_t position;
    int32_t speed;
    int16_t length;
    int16_t counter;
    uint8_t pan;
    uint8_t active;
} Grain;

typedef struct {
    uint32_t sample_rate;
    const int16_t *sample_buffer;
    uint32_t sample_length;
    uint16_t macro_scan;
    uint16_t macro_size;
    uint16_t macro_density;
    uint16_t macro_spray;
    uint32_t current_speed; 
    uint32_t spawn_counter;
    uint32_t rng_state;     
    uint8_t gate; // Tracks if a key is held down
    Grain grains[MAX_GRAINS];
} GranularVoiceState;

void granular_init(void *state, uint32_t sample_rate);
void granular_note_on(void *state, uint8_t note, uint8_t vel);
void granular_note_off(void *state, uint8_t note);
void granular_set_macro(void *state, uint8_t knob_idx, int16_t val);
void granular_render_block(void *state, int16_t *out_l, int16_t *out_r, size_t frames);
void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length);
void granular_set_active_slot(uint8_t slot);
uint8_t granular_get_active_slot(void);

#endif
