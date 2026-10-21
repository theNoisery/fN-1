/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine_granular.h"
#include <string.h>

static int16_t hann_window[1024];
static int hann_initialized = 0;

static const int16_t *global_samples[16] = {NULL};
static uint32_t global_lengths[16] = {0};
static uint8_t active_sample_slot = 0;

void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length) {
    if (slot < 16) {
        global_samples[slot] = buffer;
        global_lengths[slot] = length;
    }
}

void granular_set_active_slot(uint8_t slot) {
    if (slot < 16) active_sample_slot = slot;
}

uint8_t granular_get_active_slot(void) {
    return active_sample_slot;
}

void granular_init(void *state_ptr, uint32_t sample_rate) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(state, 0, sizeof(GranularVoiceState));
    state->sample_rate = sample_rate;
    state->macro_size = 512;
    state->macro_density = 64;
    state->current_speed = 0x10000;
    state->rng_state = 0x12345678;

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
    (void)vel;
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    
    static const uint32_t pitch_table[12] = {
        65536, 69433, 73562, 77936, 82570, 87481,
        92682, 98193, 104032, 110211, 116752, 123677
    };
    
    // Set Base Note to C2 (MIDI Note 36)
    int octave = (note / 12) - 3; 
    uint32_t base_speed = pitch_table[note % 12];
    
    if (octave > 0) state->current_speed = base_speed << octave;
    else if (octave < 0) state->current_speed = base_speed >> (-octave);
    else state->current_speed = base_speed;
}

void granular_note_off(void *state_ptr, uint8_t note) {
    (void)state_ptr; (void)note;
}

static inline uint32_t fast_rand(uint32_t *rng) {
    *rng ^= *rng << 13;
    *rng ^= *rng >> 17;
    *rng ^= *rng << 5;
    return *rng;
}

void granular_render_block(void *state_ptr, int16_t *out_l, int16_t *out_r, size_t frames) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(out_l, 0, frames * sizeof(int16_t));
    memset(out_r, 0, frames * sizeof(int16_t));

    state->sample_buffer = global_samples[active_sample_slot];
    state->sample_length = global_lengths[active_sample_slot];

    if (!state->sample_buffer || state->sample_length == 0) return;

    for (size_t f = 0; f < frames; f++) {
        state->spawn_counter++;
        uint32_t spawn_thresh = 2000 - (state->macro_density * 9); 
        if (spawn_thresh < 50) spawn_thresh = 50; 

        if (state->spawn_counter >= spawn_thresh) {
            state->spawn_counter = 0;
            for (int i = 0; i < MAX_GRAINS; i++) {
                if (!state->grains[i].active) {
                    state->grains[i].active = 1;
                    state->grains[i].counter = 0;
                    
                    state->grains[i].length = (int16_t)((state->macro_size * state->sample_rate) / 10000);
                    if (state->grains[i].length < 100) state->grains[i].length = 100;
                    
                    uint32_t base_pos = ((uint32_t)state->macro_scan * state->sample_length) >> 10;
                    
                    if (state->macro_spray > 0) {
                        uint32_t jitter_range = ((uint32_t)state->macro_spray * state->sample_length) >> 11; 
                        if (jitter_range > 0) {
                            int32_t jitter = (fast_rand(&state->rng_state) % (jitter_range * 2)) - jitter_range;
                            int32_t final_pos = (int32_t)base_pos + jitter;
                            if (final_pos < 0) final_pos += state->sample_length;
                            else if ((uint32_t)final_pos >= state->sample_length) final_pos -= state->sample_length;
                            base_pos = (uint32_t)final_pos;
                        }
                    }

                    state->grains[i].position = base_pos << 16; 
                    state->grains[i].speed = state->current_speed; 
                    break;
                }
            }
        }

        int32_t sample_sum = 0;
        int active_count = 0;
        
        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;
            active_count++;

            uint32_t index = (g->position >> 16) % state->sample_length;
            int16_t raw = state->sample_buffer[index];
            int win_idx = (g->counter * 1024) / g->length;
            if (win_idx > 1023) win_idx = 1023;
            
            sample_sum += ((int32_t)raw * hann_window[win_idx]) >> 15;
            
            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        if (active_count > 0) {
            // Divide by 4 to scale amplitude, then clamp to absolutely prevent noise
            int32_t mixed = sample_sum / 4; 
            if (mixed > 32767) mixed = 32767;
            else if (mixed < -32768) mixed = -32768;
            
            out_l[f] = (int16_t)mixed;
            out_r[f] = (int16_t)mixed;
        }
    }
}
