/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ENGINE_GRANULAR_H
#define FM1_ENGINE_GRANULAR_H

#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 8

typedef struct {
    int32_t position;
    int32_t speed;
    int16_t length;
    int16_t counter;
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
    uint32_t spawn_counter;
    Grain grains[MAX_GRAINS];
} GranularVoiceState;

void granular_init(void *state, uint32_t sample_rate);
void granular_note_on(void *state, uint8_t note, uint8_t vel);
void granular_note_off(void *state, uint8_t note);
void granular_set_macro(void *state, uint8_t knob_idx, int16_t val);
void granular_render_block(void *state, int16_t *out_l, int16_t *out_r, size_t frames);

#endif
