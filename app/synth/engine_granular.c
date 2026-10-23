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

// Dedicated FX buffers (Chorus, Stereo Delay, Schroeder Stereo Reverb)
#define CHORUS_BUF_LEN 1024
static int16_t chorus_buf_l[CHORUS_BUF_LEN];
static int16_t chorus_buf_r[CHORUS_BUF_LEN];
static uint32_t chorus_write_idx = 0;
static uint32_t chorus_lfo_phase = 0;

#define DELAY_BUF_LEN 16384
static int16_t delay_buf_l[DELAY_BUF_LEN];
static int16_t delay_buf_r[DELAY_BUF_LEN];
static uint32_t delay_write_idx = 0;

#define COMB1_LEN 1116
#define COMB2_LEN 1188
#define COMB3_LEN 1277
#define COMB4_LEN 1356
#define AP1_LEN   556
#define AP2_LEN   441
static int16_t rev_c1[COMB1_LEN], rev_c2[COMB2_LEN], rev_c3[COMB3_LEN], rev_c4[COMB4_LEN];
static int16_t rev_ap1[AP1_LEN], rev_ap2[AP2_LEN];
static uint16_t idx_c1 = 0, idx_c2 = 0, idx_c3 = 0, idx_c4 = 0, idx_ap1 = 0, idx_ap2 = 0;

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
    state->macro_scan = 1;       // 1%
    state->macro_size = 200;     // 200 ms
    state->macro_density = 12;   // 12 grains
    state->macro_spray = 10;     // 10% spray

    // Default Master ADSR in ms (0-2000ms) and dB (-60 to 0dB)
    state->env_attack = 150;     // 150 ms
    state->env_decay = 300;      // 300 ms
    state->env_sustain = 0;      // 0 dB (full sustain)
    state->env_release = 600;    // 600 ms
    state->env_stage = GRAN_ENV_IDLE;
    state->env_level = 0;

    // Default Filter & Chorus
    state->filter_mode = GRAN_FILT_LPF;
    state->filter_cutoff = 120;
    state->filter_res = 15;
    state->chorus_depth = 30;    // 30% stereo chorus

    state->current_speed = 0x10000;
    state->rng_state = 0x12345678;
    state->gate = 0;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            int32_t x = i - 512;
            int32_t val = 32767 - ((x * x) >> 3);
            if (val < 0) val = 0;
            hann_window[i] = (int16_t)val;
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
    if (knob_idx == 0) state->env_attack = (uint16_t)(val < 0 ? 0 : val > 2000 ? 2000 : val);
    else if (knob_idx == 1) state->env_decay = (uint16_t)(val < 0 ? 0 : val > 2000 ? 2000 : val);
    else if (knob_idx == 2) state->env_sustain = (int16_t)(val < -60 ? -60 : val > 0 ? 0 : val);
    else if (knob_idx == 3) state->env_release = (uint16_t)(val < 0 ? 0 : val > 2000 ? 2000 : val);
}

void granular_set_filter(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->filter_mode = (uint8_t)(val < 0 ? 0 : val > 3 ? 3 : val);
    else if (knob_idx == 1) state->filter_cutoff = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
    else if (knob_idx == 2) state->filter_res = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
    else if (knob_idx == 3) state->chorus_depth = (uint8_t)(val < 0 ? 0 : val > 100 ? 100 : val);
}

void granular_set_fx(void *state_ptr, uint8_t rev_send, uint8_t dly_send,
                     uint8_t rev_time, uint8_t dly_fb, uint8_t fx_ret, uint32_t dly_ticks) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    state->rev_send = rev_send;
    state->dly_send = dly_send;
    state->rev_time = rev_time;
    state->dly_feedback = dly_fb;
    state->fx_return = fx_ret;
    // Scale 44.1kHz delay ticks to 32kHz sample frames
    uint32_t samps = (dly_ticks * 32U) / 44U;
    if (samps < 256U) samps = 256U;
    if (samps >= DELAY_BUF_LEN - 32U) samps = DELAY_BUF_LEN - 32U;
    state->dly_samples = samps;
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

static inline int16_t soft_clip(int32_t x) {
    if (x > 48000) return 32767;
    if (x < -48000) return -32768;
    // Smooth cubic saturation above +/- 24000
    if (x > 24000) {
        int32_t over = x - 24000;
        return (int16_t)(24000 + ((over * 8767) / 24000));
    }
    if (x < -24000) {
        int32_t over = x + 24000;
        return (int16_t)(-24000 + ((over * 8768) / 24000));
    }
    return (int16_t)x;
}

// Convert -60..0 dB to linear Q16 (0..65535)
static uint32_t db_to_q16(int16_t db) {
    if (db <= -60) return 0U;
    if (db >= 0) return 65535U;
    static const uint16_t db_table[6] = {65535, 58409, 52057, 46396, 41350, 36854};
    unsigned atten = (unsigned)(-db);
    return (uint32_t)(db_table[atten % 6U] >> (atten / 6U));
}

void granular_render_block(void *state_ptr, int16_t *out_l, int16_t *out_r, size_t frames) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(out_l, 0, frames * sizeof(int16_t));
    memset(out_r, 0, frames * sizeof(int16_t));

    if (decoded_length < 64) return;

    // 1. SIZE in exact milliseconds (10ms to 500ms)
    uint32_t size_ms = state->macro_size < 10 ? 10 : state->macro_size > 500 ? 500 : state->macro_size;
    uint32_t grain_len = (size_ms * state->sample_rate) / 1000U;
    if (grain_len >= decoded_length) grain_len = decoded_length - 1U;

    // 2. DENSITY in exact simultaneous overlapping grains (1 to 24)
    uint32_t target_overlap = state->macro_density < 1 ? 1 : state->macro_density > MAX_GRAINS ? MAX_GRAINS : state->macro_density;
    uint32_t base_interval = grain_len / target_overlap;
    if (base_interval < 16U) base_interval = 16U;

    // 3. Master ADSR rates from exact milliseconds (0-2000ms) and dB (-60..0dB)
    uint32_t frames_per_ms = state->sample_rate / 1000U; // 32 frames/ms at 32kHz
    uint32_t att_step = state->env_attack == 0 ? 65535U : (65535U / ((uint32_t)state->env_attack * frames_per_ms));
    uint32_t dec_step = state->env_decay == 0  ? 65535U : (65535U / ((uint32_t)state->env_decay * frames_per_ms));
    uint32_t sus_target = db_to_q16(state->env_sustain);
    uint32_t rel_step = state->env_release == 0 ? 65535U : (65535U / ((uint32_t)state->env_release * frames_per_ms));
    if (att_step == 0) att_step = 1U;
    if (dec_step == 0) dec_step = 1U;
    if (rel_step == 0) rel_step = 1U;

    // 4. Filter coefficients
    int32_t svf_f = 512 + ((int32_t)state->filter_cutoff * 185);
    int32_t svf_q = 32767 - ((int32_t)state->filter_res * 225);

    // 5. FX parameters
    int32_t rev_fb = 19000 + ((int32_t)state->rev_time * 95); // Up to 0.96 decay
    if (rev_fb > 31200) rev_fb = 31200;
    int32_t dly_fb = ((int32_t)state->dly_feedback * 290);    // Up to 0.90 feedback
    if (dly_fb > 29500) dly_fb = 29500;
    uint32_t dly_len = state->dly_samples ? state->dly_samples : 9600U;

    // Usable scan range so a grain at 100% doesn't wrap around to 0%
    uint32_t max_start = decoded_length > grain_len ? (decoded_length - grain_len) : 1U;

    for (size_t f = 0; f < frames; f++) {
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

                        // SCAN: Locks strictly onto the chosen slice (1% - 100%)
                        uint32_t scan_pct = state->macro_scan < 1 ? 1 : state->macro_scan > 100 ? 100 : state->macro_scan;
                        uint32_t base_pos = ((scan_pct - 1U) * max_start) / 99U;

                        // SPRAY: 0% = tight 64-frame (~2ms) anti-comb smear; 100% = wide cloud
                        uint32_t spray_pct = state->macro_spray > 100 ? 100 : state->macro_spray;
                        uint32_t jitter_range = 64U + ((spray_pct * (decoded_length / 3U)) / 100U);

                        int32_t jitter = (int32_t)(fast_rand(&state->rng_state) % (jitter_range * 2U + 1U)) - (int32_t)jitter_range;
                        int32_t final_pos = (int32_t)base_pos + jitter;
                        if (final_pos < 0) final_pos = 0;
                        if ((uint32_t)final_pos > max_start) final_pos = (int32_t)max_start;

                        int32_t drift = (int32_t)(fast_rand(&state->rng_state) & 255U) - 128;
                        uint32_t speed = (uint32_t)((int32_t)state->current_speed + (((int32_t)state->current_speed * drift) >> 16));

                        state->grains[i].position = (uint32_t)final_pos << 16;
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

            uint32_t pos_int = g->position >> 16;
            if (pos_int >= decoded_length - 1U) {
                pos_int = decoded_length - 2U;
            }
            uint32_t pos_frac = g->position & 0xFFFFU;

            int32_t s0 = decoded_ram_buffer[pos_int];
            int32_t s1 = decoded_ram_buffer[pos_int + 1U];
            int32_t sample = s0 + (((s1 - s0) * (int32_t)pos_frac) >> 16);

            uint32_t win_idx = (g->counter << 10) / g->length;
            if (win_idx > 1023U) win_idx = 1023U;

            int32_t windowed = (sample * hann_window[win_idx]) >> 15;

            // Boosted constant-power-style stereo panning
            sum_l += (windowed * (int32_t)(320U - g->pan)) >> 8;
            sum_r += (windowed * (int32_t)(64U + g->pan)) >> 8;

            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        // Strong makeup gain with smooth overlap scaling
        int32_t env = (int32_t)(state->env_level >> 8);
        int32_t norm_div = (int32_t)(1U + (target_overlap >> 3));

        int32_t mixed_l = (((sum_l * 2) / norm_div) * env) >> 8;
        int32_t mixed_r = (((sum_r * 2) / norm_div) * env) >> 8;

        // 1. Multimode State Variable Filter
        if (state->filter_mode != GRAN_FILT_OFF) {
            int32_t in_l = soft_clip(mixed_l);
            int32_t in_r = soft_clip(mixed_r);

            int32_t high_l = in_l - state->svf_low_l - ((svf_q * state->svf_band_l) >> 15);
            state->svf_band_l += (svf_f * high_l) >> 15;
            state->svf_low_l += (svf_f * state->svf_band_l) >> 15;

            int32_t high_r = in_r - state->svf_low_r - ((svf_q * state->svf_band_r) >> 15);
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

        // 2. Stereo Chorus (modulated delay lines)
        if (state->chorus_depth > 0) {
            chorus_buf_l[chorus_write_idx] = soft_clip(mixed_l);
            chorus_buf_r[chorus_write_idx] = soft_clip(mixed_r);

            chorus_lfo_phase += 2U; // ~0.8 Hz triangle LFO
            int32_t tri = (int32_t)(chorus_lfo_phase & 0xFFFFU);
            if (tri > 32767) tri = 65535 - tri;
            uint32_t mod = ((uint32_t)tri * state->chorus_depth) >> 9; // Up to ~320 frames

            uint32_t tap_l = (chorus_write_idx + CHORUS_BUF_LEN - (160U + mod)) & (CHORUS_BUF_LEN - 1U);
            uint32_t tap_r = (chorus_write_idx + CHORUS_BUF_LEN - (480U - mod)) & (CHORUS_BUF_LEN - 1U);

            mixed_l += (chorus_buf_l[tap_l] * (int32_t)state->chorus_depth) / 140;
            mixed_r += (chorus_buf_r[tap_r] * (int32_t)state->chorus_depth) / 140;
            chorus_write_idx = (chorus_write_idx + 1U) & (CHORUS_BUF_LEN - 1U);
        }

        int32_t dry_l = soft_clip(mixed_l);
        int32_t dry_r = soft_clip(mixed_r);

        // 3. Stereo Delay (controlled by FX screen DLY SEND, DLY TIME, FEEDBACK)
        int32_t dly_out_l = 0, dly_out_r = 0;
        if (state->dly_send > 0) {
            uint32_t read_idx = (delay_write_idx + DELAY_BUF_LEN - dly_len) & (DELAY_BUF_LEN - 1U);
            dly_out_l = delay_buf_l[read_idx];
            dly_out_r = delay_buf_r[read_idx];

            int32_t send_l = (dry_l * (int32_t)state->dly_send) >> 4;
            int32_t send_r = (dry_r * (int32_t)state->dly_send) >> 4;

            // Cross-feedback for wide stereo ping-pong repeats
            delay_buf_l[delay_write_idx] = soft_clip(send_l + ((dly_out_r * dly_fb) >> 15));
            delay_buf_r[delay_write_idx] = soft_clip(send_r + ((dly_out_l * dly_fb) >> 15));
            delay_write_idx = (delay_write_idx + 1U) & (DELAY_BUF_LEN - 1U);
        }

        // 4. Stereo Lush Reverb (controlled by FX screen REV SEND and REV TIME)
        int32_t rev_out_l = 0, rev_out_r = 0;
        if (state->rev_send > 0) {
            int32_t rev_in = (((dry_l + dry_r) >> 1) * (int32_t)state->rev_send) >> 5;
            rev_in += ((dly_out_l + dly_out_r) >> 3); // Feed delay into reverb

            int16_t c1 = rev_c1[idx_c1]; rev_c1[idx_c1] = soft_clip(rev_in + ((c1 * rev_fb) >> 15));
            if (++idx_c1 >= COMB1_LEN) idx_c1 = 0;

            int16_t c2 = rev_c2[idx_c2]; rev_c2[idx_c2] = soft_clip(rev_in + ((c2 * rev_fb) >> 15));
            if (++idx_c2 >= COMB2_LEN) idx_c2 = 0;

            int16_t c3 = rev_c3[idx_c3]; rev_c3[idx_c3] = soft_clip(rev_in + ((c3 * rev_fb) >> 15));
            if (++idx_c3 >= COMB3_LEN) idx_c3 = 0;

            int16_t c4 = rev_c4[idx_c4]; rev_c4[idx_c4] = soft_clip(rev_in + ((c4 * rev_fb) >> 15));
            if (++idx_c4 >= COMB4_LEN) idx_c4 = 0;

            int32_t comb_sum = (c1 + c2 + c3 + c4) >> 1;

            int16_t b1 = rev_ap1[idx_ap1];
            int32_t a1 = comb_sum + (b1 >> 1);
            rev_ap1[idx_ap1] = soft_clip(a1);
            int32_t ap1_out = b1 - (a1 >> 1);
            if (++idx_ap1 >= AP1_LEN) idx_ap1 = 0;

            int16_t b2 = rev_ap2[idx_ap2];
            int32_t a2 = ap1_out + (b2 >> 1);
            rev_ap2[idx_ap2] = soft_clip(a2);
            int32_t ap2_out = b2 - (a2 >> 1);
            if (++idx_ap2 >= AP2_LEN) idx_ap2 = 0;

            rev_out_l = ap2_out + ((c1 - c3) >> 2);
            rev_out_r = ap2_out - ((c2 - c4) >> 2);
        }

        int32_t ret_gain = state->fx_return ? state->fx_return : 100;
        int32_t wet_l = ((dly_out_l + rev_out_l) * ret_gain) / 100;
        int32_t wet_r = ((dly_out_r + rev_out_r) * ret_gain) / 100;

        out_l[f] = soft_clip(dry_l + wet_l);
        out_r[f] = soft_clip(dry_r + wet_r);
    }
}
