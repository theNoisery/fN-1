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

static void granular_decode_to_ram(void) {
    const uint8_t *data = global_samples[active_sample_slot];
    uint32_t frames = global_lengths[active_sample_slot];

    if (!data || !frames) {
        decoded_length = 0;
        return;
    }

    if (frames > MAX_RAM_FRAMES) frames = MAX_RAM_FRAMES;

    int32_t decode_state[8];
    memset(decode_state, 0, sizeof(decode_state));

    unsigned chunks = (frames + 319U) / 320U;
    uint32_t frame_idx = 0;

    for (unsigned chunk = 0; chunk < chunks; chunk++) {
        size_t offset = (size_t)chunk * 228U;

        for (unsigned g = 0; g < 16U; g++) {
            int16_t group_pcm[FM1_SEA_GROUP];
            if (fm1_sea_decode_groups(data + offset, 228U, 5U, sample_dqt,
                                      g, g, decode_state, group_pcm)) {
                decoded_length = frame_idx;
                return;
            }

            uint32_t frames_to_copy = FM1_SEA_GROUP;
            if (frame_idx + frames_to_copy > frames) {
                frames_to_copy = frames - frame_idx;
            }

            memcpy(&decoded_ram_buffer[frame_idx], group_pcm, frames_to_copy * sizeof(int16_t));
            frame_idx += frames_to_copy;

            if (frame_idx >= frames) break;
        }
        if (frame_idx >= frames) break;
    }

    decoded_length = frame_idx;
}

void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length) {
    if (slot < 16) {
        global_samples[slot] = (const uint8_t *)buffer;
        global_lengths[slot] = length;
        if (slot == active_sample_slot) {
            granular_decode_to_ram();
        }
    }
}

void granular_set_active_slot(uint8_t slot) {
    if (slot < 16 && slot != active_sample_slot) {
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

    // Calibrated defaults
    state->macro_scan = 1;       // 1% (Start of sample)
    state->macro_size = 250;     // 250 ms
    state->macro_density = 12;   // 12 overlapping grains
    state->macro_spray = 25;     // 25% spray

    // Default Master ADSR (0-127)
    state->env_attack = 20;
    state->env_decay = 40;
    state->env_sustain = 100;
    state->env_release = 50;
    state->env_stage = GRAN_ENV_IDLE;
    state->env_level = 0;

    // Default Filter
    state->filter_mode = GRAN_FILT_LPF;
    state->filter_cutoff = 110;
    state->filter_res = 20;

    state->current_speed = 0x10000;
    state->rng_state = 0x12345678;
    state->gate = 0;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            int32_t x = i - 512;
            int32_t parabola = 32767 - ((x * x) >> 3);
            if (parabola < 0) parabola = 0;
            hann_window[i] = (int16_t)((parabola * parabola) >> 15);
        }
        hann_initialized = 1;
    }
    granular_decode_to_ram();
}

void granular_set_macro(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->macro_scan = (uint16_t)(val < 1 ? 1 : val > 100 ? 100 : val);
    else if (knob_idx == 1) state->macro_size = (uint16_t)(val < 10 ? 10 : val > 500 ? 500 : val);
    else if (knob_idx == 2) state->macro_density = (uint16_t)(val < 1 ? 1 : val > MAX_GRAINS ? MAX_GRAINS : val);
    else if (knob_idx == 3) state->macro_spray = (uint16_t)(val < 0 ? 0 : val > 100 ? 100 : val);
}

void granular_set_env(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    uint8_t clamped = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
    if (knob_idx == 0) state->env_attack = clamped;
    else if (knob_idx == 1) state->env_decay = clamped;
    else if (knob_idx == 2) state->env_sustain = clamped;
    else if (knob_idx == 3) state->env_release = clamped;
}

void granular_set_filter(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->filter_mode = (uint8_t)(val < 0 ? 0 : val > 3 ? 3 : val);
    else if (knob_idx == 1) state->filter_cutoff = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
    else if (knob_idx == 2) state->filter_res = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
}

void granular_note_on(void *state_ptr, uint8_t note, uint8_t vel) {
    (void)vel;
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;

    static const uint32_t pitch_table[12] = {
        65536, 69433, 73562, 77936, 82570, 87481,
        92682, 98193, 104032, 110211, 116752, 123677
    };

    int octave = ((int)note / 12) - 3;
    uint32_t base_speed = pitch_table[note % 12];

    if (octave > 0) state->current_speed = base_speed << octave;
    else if (octave < 0) state->current_speed = base_speed >> (-octave);
    else state->current_speed = base_speed;

    if (!state->gate && state->env_stage == GRAN_ENV_IDLE) {
        state->spawn_counter = state->next_spawn_delay;
        state->playhead_drift = 0;
    }
    state->gate = 1;
    state->env_stage = GRAN_ENV_ATTACK;
}

void granular_note_off(void *state_ptr, uint8_t note) {
    (void)note;
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    state->gate = 0;
    if (state->env_stage != GRAN_ENV_IDLE) {
        state->env_stage = GRAN_ENV_RELEASE;
    }
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

    if (decoded_length < 64) return;

    // 1. SIZE in exact milliseconds (10ms to 500ms -> 320 to 16000 frames at 32kHz)
    uint32_t size_ms = state->macro_size < 10 ? 10 : state->macro_size > 500 ? 500 : state->macro_size;
    uint32_t grain_len = (size_ms * state->sample_rate) / 1000U;

    // 2. DENSITY in exact simultaneous overlapping grains (1 to 24)
    uint32_t target_overlap = state->macro_density < 1 ? 1 : state->macro_density > MAX_GRAINS ? MAX_GRAINS : state->macro_density;
    uint32_t base_interval = grain_len / target_overlap;
    if (base_interval < 16U) base_interval = 16U;

    // 3. Master ADSR rates
    uint32_t att_step = 65536U / (32U + (uint32_t)state->env_attack * 512U);
    uint32_t dec_step = 65536U / (32U + (uint32_t)state->env_decay * 512U);
    uint32_t sus_target = ((uint32_t)state->env_sustain * 65535U) / 127U;
    uint32_t rel_step = 65536U / (32U + (uint32_t)state->env_release * 768U);
    if (att_step == 0) att_step = 1;
    if (dec_step == 0) dec_step = 1;
    if (rel_step == 0) rel_step = 1;

    // 4. State Variable Filter coefficients (Q15 fixed-point)
    int32_t svf_f = 512 + ((int32_t)state->filter_cutoff * 180); // Cutoff
    int32_t svf_q = 32767 - ((int32_t)state->filter_res * 220);  // Damping

    for (size_t f = 0; f < frames; f++) {
        // Advance Master ADSR Envelope
        switch (state->env_stage) {
            case GRAN_ENV_ATTACK:
                state->env_level += att_step;
                if (state->env_level >= 65535U) {
                    state->env_level = 65535U;
                    state->env_stage = GRAN_ENV_DECAY;
                }
                break;
            case GRAN_ENV_DECAY:
                if (state->env_level > sus_target + dec_step) {
                    state->env_level -= dec_step;
                } else {
                    state->env_level = sus_target;
                    state->env_stage = GRAN_ENV_SUSTAIN;
                }
                break;
            case GRAN_ENV_SUSTAIN:
                state->env_level = sus_target;
                break;
            case GRAN_ENV_RELEASE:
                if (state->env_level > rel_step) {
                    state->env_level -= rel_step;
                } else {
                    state->env_level = 0U;
                    state->env_stage = GRAN_ENV_IDLE;
                }
                break;
            default:
                state->env_level = 0U;
                break;
        }

        if (state->env_stage != GRAN_ENV_IDLE) {
            state->playhead_drift += (state->current_speed >> 4);

            state->spawn_counter++;
            if (state->spawn_counter >= state->next_spawn_delay) {
                state->spawn_counter = 0;

                uint32_t timing_jitter = fast_rand(&state->rng_state) % (base_interval / 2U + 1U);
                state->next_spawn_delay = (base_interval * 3U) / 4U + timing_jitter;

                for (int i = 0; i < MAX_GRAINS; i++) {
                    if (!state->grains[i].active) {
                        state->grains[i].active = 1;
                        state->grains[i].counter = 0;
                        state->grains[i].length = grain_len;

                        uint32_t rand_pan = fast_rand(&state->rng_state) & 127U;
                        state->grains[i].pan = (uint8_t)(64U + rand_pan);

                        // SCAN: 1% to 100% mapped across sample length
                        uint32_t scan_pct = state->macro_scan < 1 ? 1 : state->macro_scan > 100 ? 100 : state->macro_scan;
                        uint32_t scan_offset = ((scan_pct - 1U) * decoded_length) / 100U;
                        uint32_t base_pos = (scan_offset + (state->playhead_drift >> 16)) % decoded_length;

                        // SPRAY: 0% to 100% mapped up to 50% of total sample length
                        uint32_t spray_pct = state->macro_spray > 100 ? 100 : state->macro_spray;
                        uint32_t jitter_range = 256U + ((spray_pct * (decoded_length / 2U)) / 100U);

                        int32_t jitter = (int32_t)(fast_rand(&state->rng_state) % (jitter_range * 2U + 1U)) - (int32_t)jitter_range;
                        int32_t final_pos = (int32_t)base_pos + jitter;
                        while (final_pos < 0) final_pos += (int32_t)decoded_length;
                        uint32_t wrapped_pos = (uint32_t)final_pos % decoded_length;

                        int32_t drift = (int32_t)(fast_rand(&state->rng_state) & 511U) - 256;
                        uint32_t speed = (uint32_t)((int32_t)state->current_speed + (((int32_t)state->current_speed * drift) >> 16));

                        state->grains[i].position = wrapped_pos << 16;
                        state->grains[i].speed = speed;
                        break;
                    }
                }
            }
        }

        int32_t sum_l = 0;
        int32_t sum_r = 0;

        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;

            uint32_t pos_int = (g->position >> 16) % decoded_length;
            uint32_t pos_frac = g->position & 0xFFFFU;

            int32_t s0 = decoded_ram_buffer[pos_int];
            int32_t s1 = decoded_ram_buffer[(pos_int + 1U) % decoded_length];
            int32_t sample = s0 + (((s1 - s0) * (int32_t)pos_frac) >> 16);

            uint32_t win_idx = (g->counter << 10) / g->length;
            if (win_idx > 1023U) win_idx = 1023U;

            int32_t windowed = (sample * hann_window[win_idx]) >> 15;

            sum_l += (windowed * (int32_t)(255U - g->pan)) >> 8;
            sum_r += (windowed * (int32_t)g->pan) >> 8;

            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        int32_t env = (int32_t)(state->env_level >> 8);
        int32_t norm_div = (int32_t)(2U + (target_overlap >> 2));

        int32_t mixed_l = ((sum_l / norm_div) * env) >> 8;
        int32_t mixed_r = ((sum_r / norm_div) * env) >> 8;

        // Stereo Multimode Filter (Chamberlin SVF)
        if (state->filter_mode != GRAN_FILT_OFF) {
            int32_t high_l = mixed_l - state->svf_low_l - ((svf_q * state->svf_band_l) >> 15);
            state->svf_band_l += (svf_f * high_l) >> 15;
            state->svf_low_l += (svf_f * state->svf_band_l) >> 15;

            int32_t high_r = mixed_r - state->svf_low_r - ((svf_q * state->svf_band_r) >> 15);
            state->svf_band_r += (svf_f * high_r) >> 15;
            state->svf_low_r += (svf_f * state->svf_band_r) >> 15;

            if (state->filter_mode == GRAN_FILT_LPF) {
                mixed_l = state->svf_low_l;
                mixed_r = state->svf_low_r;
            } else if (state->filter_mode == GRAN_FILT_BPF) {
                mixed_l = state->svf_band_l;
                mixed_r = state->svf_band_r;
            } else if (state->filter_mode == GRAN_FILT_HPF) {
                mixed_l = high_l;
                mixed_r = high_r;
            }
        }

        if (mixed_l > 32767) mixed_l = 32767;
        else if (mixed_l < -32768) mixed_l = -32768;

        if (mixed_r > 32767) mixed_r = 32767;
        else if (mixed_r < -32768) mixed_r = -32768;

        out_l[f] = (int16_t)mixed_l;
        out_r[f] = (int16_t)mixed_r;
    }
}
