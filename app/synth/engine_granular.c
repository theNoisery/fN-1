/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine_granular.h"
#include <string.h>

static int16_t hann_window[1024];
static int hann_initialized = 0;

// Global sample pointer shared across granular voices
static const int16_t *global_sample_buffer = NULL;
static uint32_t global_sample_length = 0;

void granular_global_sample_set(const int16_t *buffer, uint32_t length) {
    global_sample_buffer = buffer;
    global_sample_length = length;
}

void granular_init(void *state_ptr, uint32_t sample_rate) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(state, 0, sizeof(GranularVoiceState));
    state->sample_rate = sample_rate;
    state->macro_size = 512;
    state->macro_density = 64;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            int32_t val = 32767 - (((i - 512) * (i - 512) * 32767) >> 18);
            if (val < 0) val = 0;
            hann_window[i] = (int16_t)val;
        }
        hann_initialized = 1;
    }
}

void granular_set_macro(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->macro_scan = val;
    else if (knob_idx == 1) state->macro_size = val;
    else if (knob_idx == 2) state->macro_density = val;
    else if (knob_idx == 3) state->macro_spray = val;
}

void granular_note_on(void *state_ptr, uint8_t note, uint8_t vel) {
    (void)state_ptr; (void)note; (void)vel;
}

void granular_note_off(void *state_ptr, uint8_t note) {
    (void)state_ptr; (void)note;
}

void granular_render_block(void *state_ptr, int16_t *out_l, int16_t *out_r, size_t frames) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(out_l, 0, frames * sizeof(int16_t));
    memset(out_r, 0, frames * sizeof(int16_t));

    // Pull from global sample pool if not locally set
    if (!state->sample_buffer && global_sample_buffer) {
        state->sample_buffer = global_sample_buffer;
        state->sample_length = global_sample_length;
    }

    if (!state->sample_buffer || state->sample_length == 0) return;

    for (size_t f = 0; f < frames; f++) {
        state->spawn_counter++;
        if (state->spawn_counter >= (200 - (state->macro_density >> 3))) {
            state->spawn_counter = 0;
            for (int i = 0; i < MAX_GRAINS; i++) {
                if (!state->grains[i].active) {
                    state->grains[i].active = 1;
                    state->grains[i].counter = 0;
                    state->grains[i].length = (int16_t)((state->macro_size * state->sample_rate) / 4000);
                    if (state->grains[i].length < 100) state->grains[i].length = 100;
                    
                    uint32_t base_pos = ((uint32_t)state->macro_scan * state->sample_length) >> 10;
                    state->grains[i].position = base_pos << 16; 
                    state->grains[i].speed = 0x10000;
                    break;
                }
            }
        }

        int32_t sample_sum = 0;
        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;

            uint32_t index = g->position >> 16;
            if (index >= state->sample_length) {
                g->active = 0;
                continue;
            }

            int16_t raw = state->sample_buffer[index];
            int win_idx = (g->counter * 1024) / g->length;
            if (win_idx > 1023) win_idx = 1023;
            
            sample_sum += ((int32_t)raw * hann_window[win_idx]) >> 15;
            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        out_l[f] = (int16_t)(sample_sum / (MAX_GRAINS / 2));
        out_r[f] = out_l[f];
    }
}
