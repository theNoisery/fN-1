#include "engine_granular.h"
#include <string.h>

// Pre-calculated Hann window table for smooth grain envelopes (1024 points)
static int16_t hann_window[1024];
static int hann_initialized = 0;

void granular_init(void *state_ptr, uint32_t sample_rate) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(state, 0, sizeof(GranularVoiceState));
    state->sample_rate = sample_rate;
    state->macro_size = 512; // Default size
    state->macro_density = 64;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            // Simple sine approximation for Hann window: 0.5 * (1 - cos(2*pi*i / 1024))
            // Scaled to Q15 (0 to 32767)
            float angle = (float)i * (2.0f * 3.14159265f / 1024.0f);
            float w = 0.5f * (1.0f - cosf(angle));
            hann_window[i] = (int16_t)(w * 32767.0f);
        }
        hann_initialized = 1;
    }
}

void granular_set_macro(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->macro_scan = val;     // K1
    else if (knob_idx == 1) state->macro_size = val; // K2
    else if (knob_idx == 2) state->macro_density = val;// K3
    else if (knob_idx == 3) state->macro_spray = val;  // K4
}

void granular_note_on(void *state_ptr, uint8_t note, uint8_t vel) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    // Map MIDI note to center playback speed multiplier (Q16.16)
    // Middle C (64) = 1.0 (0x10000)
}

void granular_note_off(void *state_ptr, uint8_t note) {
    // Let grains decay naturally or silence active voices
}

void granular_render_block(void *state_ptr, int16_t *out_l, int16_t *out_r, size_t frames) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(out_l, 0, frames * sizeof(int16_t));
    memset(out_r, 0, frames * sizeof(int16_t));

    if (!state->sample_buffer || state->sample_length == 0) return;

    for (size_t f = 0; f < frames; f++) {
        // 1. Check if it's time to spawn a new grain based on K3 density
        state->spawn_counter++;
        if (state->spawn_counter >= (200 - (state->macro_density >> 3))) {
            state->spawn_counter = 0;
            // Find an inactive grain slot
            for (int i = 0; i < MAX_GRAINS; i++) {
                if (!state->grains[i].active) {
                    state->grains[i].active = 1;
                    state->grains[i].counter = 0;
                    // Grain length determined by K2 (20ms to 250ms)
                    state->grains[i].length = (int16_t)((state->macro_size * state->sample_rate) / 4000);
                    if (state->grains[i].length < 100) state->grains[i].length = 100;
                    
                    // Scan position determined by K1 with slight randomization
                    uint32_t base_pos = ((uint32_t)state->macro_scan * state->sample_length) >> 10;
                    state->grains[i].position = base_pos << 16; 
                    state->grains[i].speed = 0x10000; // Normal speed
                    break;
                }
            }
        }

        // 2. Mix active grains
        int32_t sample_sum_l = 0;
        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;

            uint32_t index = g->position >> 16;
            if (index >= state->sample_length) {
                g->active = 0;
                continue;
            }

            int16_t raw_sample = state->sample_buffer[index];

            // Apply Hann window envelope lookup
            int win_idx = (g->counter * 1024) / g->length;
            if (win_idx > 1023) win_idx = 1023;
            int32_t windowed = ((int32_t)raw_sample * hann_window[win_idx]) >> 15;

            sample_sum_l += windowed;

            // Advance grain pointers
            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) {
                g->active = 0;
            }
        }

        // Output mono grain cloud to stereo buffer
        out_l[f] = (int16_t)(sample_sum_l / (MAX_GRAINS / 2));
        out_r[f] = out_l[f];
    }
}
