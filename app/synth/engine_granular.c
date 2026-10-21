/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine_granular.h"
#include "fm1_sea.h"
#include "engine.h"
#include <string.h>

#ifndef SAMPLE_DRUM_PADS
#define SAMPLE_DRUM_PADS 8
#endif

#include "sample_metadata.inc"

static int16_t hann_window[1024];
static int hann_initialized = 0;

static const uint8_t *global_samples[16] = {NULL};
static uint32_t global_lengths[16] = {0};
static uint8_t active_sample_slot = 0;

#define MAX_RAM_FRAMES 131072 
static int16_t decoded_ram_buffer[MAX_RAM_FRAMES];
static uint32_t decoded_length = 0;

void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length) {
    if (slot < 16) {
        global_samples[slot] = (const uint8_t *)buffer; 
        global_lengths[slot] = length;
    }
}

static void granular_decode_to_ram(void) {
    const uint8_t *data = global_samples[active_sample_slot];
    uint32_t frames = global_lengths[active_sample_slot];
    
    if (!data || !frames) {
        decoded_length = 0;
        return;
    }

    if (frames > MAX_RAM_FRAMES) frames = MAX_RAM_FRAMES;
    decoded_length = frames;

    int32_t decode_state[8]; 
    memset(decode_state, 0, sizeof(decode_state));

    unsigned chunks = (frames + 319) / 320;
    unsigned frame_idx = 0;

    for (unsigned chunk = 0; chunk < chunks; chunk++) {
        size_t offset = chunk * 228; 
        int16_t chunk_pcm[320];
        fm1_sea_decode_groups(data + offset, 228, 5, sample_dqt, 0, 15, decode_state, chunk_pcm);

        unsigned frames_to_copy = 320;
        if (frame_idx + 320 > frames) frames_to_copy = frames - frame_idx;
        memcpy(&decoded_ram_buffer[frame_idx], chunk_pcm, frames_to_copy * sizeof(int16_t));
        frame_idx += frames_to_copy;
        
        if (frame_idx >= frames) break;
    }
}

void granular_set_active_slot(uint8_t slot) {
    if (slot < 16) {
        active_sample_slot = slot;
        granular_decode_to_ram();
    }
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
    state->gate = 0;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            int32_t val = 32767 - (((i - 512) * (i - 512) * 32767) >> 18);
            if (val < 0) val = 0;
            hann_window[i] = (int16_t)val;
        }
        hann_initialized = 1;
    }
    granular_decode_to_ram();
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
    state->gate = 1; // Open the spawning gate
    
    static const uint32_t pitch_table[12] = {
        65536, 69433, 73562, 77936, 82570, 87481,
        92682, 98193, 104032, 110211, 116752, 123677
    };
    
    int octave = (note / 12) - 3; 
    uint32_t base_speed = pitch_table[note % 12];
    
    if (octave > 0) state->current_speed = base_speed << octave;
    else if (octave < 0) state->current_speed = base_speed >> (-octave);
    else state->current_speed = base_speed;
}

void granular_note_off(void *state_ptr, uint8_t note) {
    (void)note;
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    state->gate = 0; // Stop spawning new grains, allowing natural decay
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

    if (decoded_length == 0) return;

    for (size_t f = 0; f < frames; f++) {
        // Only spawn new grains while the key is physically held down
        if (state->gate) {
            state->spawn_counter++;
            
            uint32_t base_thresh = 250 - state->macro_density; 
            if (base_thresh < 5) base_thresh = 5; 
            
            uint32_t spawn_jitter = fast_rand(&state->rng_state) % (base_thresh / 2 + 1);
            uint32_t spawn_thresh = base_thresh + spawn_jitter;

            if (state->spawn_counter >= spawn_thresh) {
                state->spawn_counter = 0;
                for (int i = 0; i < MAX_GRAINS; i++) {
                    if (!state->grains[i].active) {
                        state->grains[i].active = 1;
                        state->grains[i].counter = 0;
                        state->grains[i].pan = fast_rand(&state->rng_state) & 255;
                        
                        state->grains[i].length = (int16_t)((state->macro_size * state->sample_rate) / 10000);
                        if (state->grains[i].length < 100) state->grains[i].length = 100;
                        
                        uint32_t base_pos = ((uint32_t)state->macro_scan * decoded_length) >> 10;
                        
                        if (state->macro_spray > 0) {
                            uint32_t jitter_range = ((uint32_t)state->macro_spray * decoded_length) >> 11; 
                            if (jitter_range > 0) {
                                int32_t jitter = (fast_rand(&state->rng_state) % (jitter_range * 2)) - jitter_range;
                                int32_t final_pos = (int32_t)base_pos + jitter;
                                if (final_pos < 0) final_pos += decoded_length;
                                else if ((uint32_t)final_pos >= decoded_length) final_pos -= decoded_length;
                                base_pos = (uint32_t)final_pos;
                            }
                        }
                        
                        int32_t pitch_drift = (fast_rand(&state->rng_state) % 2048) - 1024;
                        int32_t speed = state->current_speed + ((state->current_speed * pitch_drift) >> 16);

                        state->grains[i].position = base_pos << 16; 
                        state->grains[i].speed = speed; 
                        break;
                    }
                }
            }
        } else {
            state->spawn_counter = 0; 
        }

        int32_t sample_sum_l = 0;
        int32_t sample_sum_r = 0;
        int active_count = 0;
        
        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;
            active_count++;

            uint32_t index = (g->position >> 16) % decoded_length;
            int16_t raw = decoded_ram_buffer[index];
            int win_idx = (g->counter * 1024) / g->length;
            if (win_idx > 1023) win_idx = 1023;
            
            int32_t windowed = ((int32_t)raw * hann_window[win_idx]) >> 15;
            
            sample_sum_l += (windowed * (255 - g->pan)) >> 8;
            sample_sum_r += (windowed * g->pan) >> 8;
            
            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        if (active_count > 0) {
            // Stable scalar based entirely on the density parameter setting to eliminate zipper noise
            int scale = 2 + (state->macro_density / 35); 
            
            int32_t mixed_l = sample_sum_l / scale; 
            int32_t mixed_r = sample_sum_r / scale; 
            
            if (mixed_l > 32767) mixed_l = 32767;
            else if (mixed_l < -32768) mixed_l = -32768;
            
            if (mixed_r > 32767) mixed_r = 32767;
            else if (mixed_r < -32768) mixed_r = -32768;
            
            out_l[f] = (int16_t)mixed_l;
            out_r[f] = (int16_t)mixed_r;
        }
    }
}
