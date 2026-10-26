/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine_granular.h"
#include "fm1_sea.h"
#include "engine.h"
#include <string.h>

/* Real SEA5 dequantizer (16 * 32 entries). fm1_sea_decode_groups() indexes it
 * with the stream's 5-bit codes, so a shorter table reads past its end. */
#include "sea_dqt.inc"

/* Placement rules (see "Audio code and every callee must be safe during Flash
 * programming" in sdk/include/fm1.h):
 *  - Everything reachable from granular_render_block, granular_note_on/off,
 *    granular_all_notes_off, granular_silence and granular_is_ready is
 *    FM1_DSP_CODE, and its lookup tables are FM1_DSP_DATA.
 *  - No division, modulo, memset or memcpy there: the compiler turns those
 *    into libcompiler_rt/libc calls that live in Flash. Divisions that are
 *    needed once per block use udiv32() below; per-frame work uses
 *    multiplies and shifts.
 *  - init, set_*, sample binding and decoding run in the control task and may
 *    use Flash freely. */

static int16_t hann_window[1024];
static int hann_initialized = 0;

static const uint8_t *global_samples[GRAN_SLOTS] = {NULL};
static uint32_t global_lengths[GRAN_SLOTS] = {0};
static uint8_t active_sample_slot = 0;

/* Source frames decoded for the active slot (32 kHz). */
#define MAX_RAM_FRAMES 8192
static int16_t decoded_ram_buffer[MAX_RAM_FRAMES];
static volatile uint32_t decoded_length = 0;

/* ---- control task: sample decoding ------------------------------------ */

static void granular_decode_to_ram(void) {
    /* The audio callback treats fewer than 64 frames as "no sample", so it
     * stays silent while the buffer is rewritten. */
    decoded_length = 0;
    GRAN_NO_IDIOM();

    const uint8_t *data = global_samples[active_sample_slot];
    uint32_t frames = global_lengths[active_sample_slot];

    if (!data || !frames) return;
    if (frames > MAX_RAM_FRAMES) frames = MAX_RAM_FRAMES;

    int32_t decode_state[8];
    memset(decode_state, 0, sizeof(decode_state));

    unsigned chunks = (frames + 319U) / 320U;
    uint32_t frame_idx = 0;

    for (unsigned chunk = 0; chunk < chunks; chunk++) {
        size_t offset = (size_t)chunk * 228U;

        for (unsigned g = 0; g < 16U; g++) {
            int16_t group_pcm[FM1_SEA_GROUP];
            if (fm1_sea_decode_groups(data + offset, 228U, 5U, granular_sea_dqt,
                                      g, g, decode_state, group_pcm)) {
                GRAN_NO_IDIOM();
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

    GRAN_NO_IDIOM();
    decoded_length = frame_idx;
}

void granular_global_sample_set(uint8_t slot, const int16_t *buffer, uint32_t length) {
    if (slot < GRAN_SLOTS) {
        global_samples[slot] = (const uint8_t *)buffer;
        global_lengths[slot] = length;
        if (slot == active_sample_slot) {
            granular_decode_to_ram();
        }
    }
}

void granular_set_active_slot(uint8_t slot) {
    if (slot < GRAN_SLOTS && slot != active_sample_slot) {
        active_sample_slot = slot;
        granular_decode_to_ram();
    }
}

uint8_t granular_get_active_slot(void) {
    return active_sample_slot;
}

/* ---- control task: parameters ------------------------------------------ */

void granular_init(void *state_ptr, uint32_t sample_rate) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    memset(state, 0, sizeof(GranularVoiceState));
    state->sample_rate = sample_rate;
    state->frames_per_ms = sample_rate / 1000U;
    if (state->frames_per_ms == 0) state->frames_per_ms = GRANULAR_OUT_RATE / 1000U;

    state->macro_scan = 1;
    state->macro_size = 200;
    state->macro_density = 12;
    state->macro_spray = 10;

    state->macro_stereo = 50;
    state->macro_pitch_rnd = 0;
    state->poly_voices = 4;
    state->chorus_depth = 30;

    state->env_attack = 150;
    state->env_decay = 300;
    state->env_sustain = 0;
    state->env_release = 600;

    state->filter_mode = GRAN_FILT_LPF;
    state->filter_cutoff = 120;
    state->filter_res = 15;
    state->filter_slope = 0;
    state->volume = 100;

    state->rng_state = 0x12345678;

    if (!hann_initialized) {
        for (int i = 0; i < 1024; i++) {
            int32_t x = i - 512;
            int32_t val = 32767 - ((x * x) >> 3);
            if (val < 0) val = 0;
            hann_window[i] = (int16_t)val;
        }
        hann_initialized = 1;
    }
    /* The active slot is decoded when it is bound (granular_global_sample_set),
     * so nothing is decoded here. */
}

void granular_set_macro(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->macro_scan = (uint16_t)(val < 1 ? 1 : val > 100 ? 100 : val);
    else if (knob_idx == 1) state->macro_size = (uint16_t)(val < 10 ? 10 : val > 500 ? 500 : val);
    else if (knob_idx == 2) state->macro_density = (uint16_t)(val < 1 ? 1 : val > 24 ? 24 : val);
    else if (knob_idx == 3) state->macro_spray = (uint16_t)(val < 0 ? 0 : val > 100 ? 100 : val);
}

void granular_set_space(void *state_ptr, uint8_t knob_idx, int16_t val) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    if (knob_idx == 0) state->macro_stereo = (uint16_t)(val < 0 ? 0 : val > 100 ? 100 : val);
    else if (knob_idx == 1) state->macro_pitch_rnd = (uint16_t)(val < 0 ? 0 : val > 100 ? 100 : val);
    else if (knob_idx == 2) state->poly_voices = (uint8_t)(val < 1 ? 1 : val > GRAN_MAX_VOICES ? GRAN_MAX_VOICES : val);
    else if (knob_idx == 3) state->chorus_depth = (uint8_t)(val < 0 ? 0 : val > 100 ? 100 : val);
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
    else if (knob_idx == 3) state->filter_slope = (uint8_t)(val < 0 ? 0 : val > 2 ? 2 : val);
}

/* ---- audio callback: helpers (SRAM, no Flash callees) ------------------- */

/* Shift/subtract divide so that no libcompiler_rt routine (Flash) is needed.
 * About 130 cycles; used a handful of times per 128-frame block, never per
 * frame. */
static FM1_DSP_CODE uint32_t udiv32(uint32_t n, uint32_t d) {
    if (!d) return 0xFFFFFFFFU;
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1U);
        if (r >= d) { r -= d; q |= 1U << i; }
    }
    return q;
}

static inline __attribute__((always_inline)) uint32_t fast_rand(uint32_t *rng) {
    *rng ^= *rng << 13;
    *rng ^= *rng >> 17;
    *rng ^= *rng << 5;
    return *rng;
}

/* Uniform value in [0, n) for n <= 65535, without a modulo. */
static inline __attribute__((always_inline)) uint32_t rand_below(uint32_t *rng, uint32_t n) {
    return ((fast_rand(rng) >> 16) * n) >> 16;
}

static inline __attribute__((always_inline)) int16_t soft_clip(int32_t x) {
    if (x > 30000) return 32767;
    if (x < -30000) return -32768;
    return (int16_t)x;
}

/* Bounds the filter state so every 32-bit product below stays in range:
 * 32767 * 65535 and 24007 * 65535 are both under 2^31. Without this a high
 * Q setting lets the state grow until the products wrap and the filter
 * produces full-scale noise. */
static inline __attribute__((always_inline)) int32_t svf_limit(int32_t x) {
    if (x > 65535) return 65535;
    if (x < -65535) return -65535;
    return x;
}

/* Grain read speed (Q16 source frames per output frame) for C-semitone 0 of
 * octave 0, i.e. 32000/44100 * 2^(n/12) * 65536. Note 36 (C2) plays the
 * sample at its original pitch; sampledrum uses the same ratio. */
static FM1_DSP_DATA const uint32_t pitch_table[12] = {
    47554, 50382, 53378, 56552, 59915, 63478,
    67252, 71251, 75488, 79977, 84732, 89771
};

static FM1_DSP_CODE uint32_t note_to_speed(uint8_t note) {
    unsigned oct12 = ((unsigned)note * 171U) >> 11; /* note / 12, exact for 0..255 */
    unsigned semi = (unsigned)note - oct12 * 12U;
    int octave = (int)oct12 - 3;
    uint32_t base_speed = pitch_table[semi];
    if (octave > 0) return base_speed << octave;
    if (octave < 0) return base_speed >> (-octave);
    return base_speed;
}

void FM1_DSP_CODE granular_note_on(void *state_ptr, uint8_t note, uint8_t vel) {
    (void)vel;
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    state->voice_age_counter++;
    uint32_t speed = note_to_speed(note);

    uint8_t max_v = state->poly_voices;
    if (max_v < 1) max_v = 1;
    if (max_v > GRAN_MAX_VOICES) max_v = GRAN_MAX_VOICES;

    int chosen = -1;

    if (max_v == 1) {
        chosen = 0;
        for (int i = 1; i < GRAN_MAX_VOICES; i++) {
            state->voices[i].gate = 0;
            if (state->voices[i].active && state->voices[i].env_stage != GRAN_ENV_IDLE)
                state->voices[i].env_stage = GRAN_ENV_RELEASE;
        }
    } else {
        for (int i = 0; i < (int)max_v; i++) {
            if (state->voices[i].active && state->voices[i].note == note) {
                chosen = i;
                break;
            }
        }
        if (chosen < 0) {
            for (int i = 0; i < (int)max_v; i++) {
                if (!state->voices[i].active || state->voices[i].env_stage == GRAN_ENV_IDLE) {
                    chosen = i;
                    break;
                }
            }
        }
        if (chosen < 0) {
            uint32_t oldest_age = 0xFFFFFFFFU;
            for (int i = 0; i < (int)max_v; i++) {
                if (!state->voices[i].gate && state->voices[i].age < oldest_age) {
                    oldest_age = state->voices[i].age;
                    chosen = i;
                }
            }
        }
        if (chosen < 0) {
            uint32_t oldest_age = 0xFFFFFFFFU;
            chosen = 0;
            for (int i = 0; i < (int)max_v; i++) {
                if (state->voices[i].age < oldest_age) {
                    oldest_age = state->voices[i].age;
                    chosen = i;
                }
            }
        }
    }

    GranularPolyVoice *v = &state->voices[chosen];
    if (!v->active || v->env_stage == GRAN_ENV_IDLE) {
        v->env_level = 0;
        v->spawn_counter = v->next_spawn_delay;
    }
    v->active = 1;
    v->gate = 1;
    v->note = note;
    v->current_speed = speed;
    v->env_stage = GRAN_ENV_ATTACK;
    v->age = state->voice_age_counter;
}

void FM1_DSP_CODE granular_note_off(void *state_ptr, uint8_t note) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    for (int i = 0; i < GRAN_MAX_VOICES; i++) {
        if (state->voices[i].active && state->voices[i].note == note) {
            state->voices[i].gate = 0;
            if (state->voices[i].env_stage != GRAN_ENV_IDLE) {
                state->voices[i].env_stage = GRAN_ENV_RELEASE;
            }
        }
    }
}

void FM1_DSP_CODE granular_all_notes_off(void *state_ptr) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    for (int i = 0; i < GRAN_MAX_VOICES; i++) {
        if (state->voices[i].active) {
            state->voices[i].gate = 0;
            if (state->voices[i].env_stage != GRAN_ENV_IDLE) {
                state->voices[i].env_stage = GRAN_ENV_RELEASE;
            }
        }
    }
}

void FM1_DSP_CODE granular_silence(void *state_ptr) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    for (int i = 0; i < GRAN_MAX_VOICES; i++) {
        GranularPolyVoice *v = &state->voices[i];
        v->active = 0;
        v->gate = 0;
        v->env_stage = GRAN_ENV_IDLE;
        v->env_level = 0;
        v->spawn_counter = 0;
        v->next_spawn_delay = 0;
        GRAN_NO_IDIOM();
    }
    for (int i = 0; i < MAX_GRAINS; i++) {
        state->grains[i].active = 0;
        GRAN_NO_IDIOM();
    }
}

int FM1_DSP_CODE granular_is_ready(void) {
    return decoded_length >= 64U;
}

/* +/-2 octaves of random pitch, as a Q15 ratio table indexed by semitone. */
static FM1_DSP_DATA const uint32_t semitone_q15[25] = {
    16384, 17358, 18390, 19484, 20643, 21870,
    23170, 24548, 26008, 27554, 29193, 30929,
    32768,
    34716, 36781, 38968, 41285, 43740, 46341,
    49097, 52016, 55109, 58386, 61858, 65536
};

static FM1_DSP_CODE uint32_t randomize_grain_speed(uint32_t base_speed, uint16_t pitch_rnd_pct, uint32_t *rng) {
    if (pitch_rnd_pct == 0) return base_speed;
    if (pitch_rnd_pct > 100) pitch_rnd_pct = 100;

    uint32_t max_cents = (uint32_t)pitch_rnd_pct * 12U;
    int32_t rand_cents = (int32_t)rand_below(rng, max_cents * 2U + 1U) - (int32_t)max_cents;
    uint32_t c_pos = (uint32_t)(rand_cents + 1200);
    if (c_pos > 2400U) c_pos = 2400U;

    uint32_t semi = (c_pos * 5243U) >> 19; /* c_pos / 100, exact for 0..2400 */
    uint32_t frac = c_pos - semi * 100U;
    uint32_t r0 = semitone_q15[semi];
    uint32_t r1 = semitone_q15[semi < 24U ? semi + 1U : 24U];
    uint32_t ratio_q15 = r0 + (((r1 - r0) * frac * 656U) >> 16); /* frac / 100 */

    /* (base_speed * ratio_q15) >> 15 without a 64-bit product (that would be a
     * Flash call on this core). Pre-shift only as much as the magnitude needs
     * so low notes keep their precision: ratio_q15 <= 2^16. */
    if (base_speed < (1U << 15)) return (base_speed * ratio_q15) >> 15;
    if (base_speed < (1U << 20)) return ((base_speed >> 5) * ratio_q15) >> 10;
    return ((base_speed >> 9) * ratio_q15) >> 6;
}

static FM1_DSP_DATA const uint16_t db_table[6] = {65535, 58409, 52057, 46396, 41350, 36854};

static FM1_DSP_CODE uint32_t db_to_q16(int16_t db) {
    if (db <= -60) return 0U;
    if (db >= 0) return 65535U;
    unsigned atten = (unsigned)(-db);               /* 1..59 */
    unsigned halves = (atten * 171U) >> 10;         /* atten / 6, exact below 128 */
    return (uint32_t)(db_table[atten - halves * 6U] >> halves);
}

static FM1_DSP_DATA const uint8_t pairs_per_voice[GRAN_MAX_VOICES + 1] = {24, 24, 12, 8, 6, 4, 4};

static FM1_DSP_CODE void spawn_single_grain(GranularVoiceState *state, uint8_t voice_idx,
                                            uint32_t pos, uint32_t speed, uint32_t grain_len,
                                            uint32_t win_inc, uint16_t start_delay, uint8_t pan) {
    for (int i = 0; i < MAX_GRAINS; i++) {
        if (!state->grains[i].active) {
            state->grains[i].active = 1;
            state->grains[i].voice_idx = voice_idx;
            state->grains[i].counter = 0;
            state->grains[i].length = grain_len;
            state->grains[i].win_pos = 0;
            state->grains[i].win_inc = win_inc;
            state->grains[i].start_delay = start_delay;
            state->grains[i].pan = pan;
            state->grains[i].position = pos << 16;
            state->grains[i].speed = speed;
            return;
        }
    }
}

void FM1_DSP_CODE granular_render_block(void *state_ptr, int16_t *out_l, int16_t *out_r, size_t frames) {
    GranularVoiceState *state = (GranularVoiceState *)state_ptr;
    const uint32_t dlen = decoded_length;

    if (dlen < 64U) {
        for (size_t f = 0; f < frames; f++) {
            out_l[f] = 0;
            out_r[f] = 0;
            GRAN_NO_IDIOM();
        }
        return;
    }

    /* ---- per-block setup (divisions here use udiv32) ---- */
    uint32_t size_ms = state->macro_size < 10 ? 10 : state->macro_size > 500 ? 500 : state->macro_size;
    uint32_t grain_len = udiv32(size_ms * state->sample_rate, 1000U);
    if (grain_len >= dlen) grain_len = dlen - 1U;
    if (grain_len < 2U) grain_len = 2U;

    uint32_t target_overlap = state->macro_density < 1 ? 1 : state->macro_density > 24 ? 24 : state->macro_density;

    uint32_t active_voices = 0;
    for (int v = 0; v < GRAN_MAX_VOICES; v++) {
        if (state->voices[v].active && state->voices[v].env_stage != GRAN_ENV_IDLE)
            active_voices++;
    }

    uint32_t max_pairs_per_voice = pairs_per_voice[active_voices];
    uint32_t per_voice_overlap = target_overlap > max_pairs_per_voice ? max_pairs_per_voice : target_overlap;

    uint32_t base_interval = udiv32(grain_len, per_voice_overlap);
    if (base_interval < 16U) base_interval = 16U;
    uint32_t win_inc = udiv32(1U << 26, grain_len); /* 1024 window entries over one grain, Q16 */

    uint32_t frames_per_ms = state->frames_per_ms ? state->frames_per_ms : GRANULAR_OUT_RATE / 1000U;
    uint32_t att_step = state->env_attack == 0 ? 65535U : udiv32(65535U, (uint32_t)state->env_attack * frames_per_ms);
    uint32_t dec_step = state->env_decay == 0  ? 65535U : udiv32(65535U, (uint32_t)state->env_decay * frames_per_ms);
    uint32_t sus_target = db_to_q16(state->env_sustain);
    uint32_t rel_step = state->env_release == 0 ? 65535U : udiv32(65535U, (uint32_t)state->env_release * frames_per_ms);
    if (att_step == 0) att_step = 1U;
    if (dec_step == 0) dec_step = 1U;
    if (rel_step == 0) rel_step = 1U;

    int32_t svf_f = 512 + ((int32_t)state->filter_cutoff * 185);
    int32_t svf_q = 32767 - ((int32_t)state->filter_res * 225);
    int32_t svf_q_follower = svf_q + ((32767 - svf_q) >> 1);
    uint8_t filter_stages = state->filter_slope > 2 ? 2 : state->filter_slope;

    uint32_t max_start = dlen > grain_len ? (dlen - grain_len) : 1U;
    uint32_t scan_pct = state->macro_scan < 1 ? 1 : state->macro_scan > 100 ? 100 : state->macro_scan;
    uint32_t base_pos = udiv32((scan_pct - 1U) * max_start, 99U);
    uint32_t spray_pct = state->macro_spray > 100 ? 100 : state->macro_spray;
    uint32_t jitter_range = 64U + udiv32(spray_pct * dlen, 300U);
    uint32_t stereo_pct = state->macro_stereo > 100 ? 100 : state->macro_stereo;
    uint32_t stereo_q8 = udiv32(stereo_pct * 256U, 100U); /* 0..256 */

    uint32_t norm_div = 2U + (per_voice_overlap >> 2) + (active_voices >> 1);
    int32_t norm_q15 = (int32_t)udiv32(32768U, norm_div);
    int32_t chorus_gain = (int32_t)state->chorus_depth * 468; /* depth / 140 in Q16 */

    for (size_t f = 0; f < frames; f++) {
        for (uint8_t v_idx = 0; v_idx < GRAN_MAX_VOICES; v_idx++) {
            GranularPolyVoice *v = &state->voices[v_idx];
            if (!v->active || v->env_stage == GRAN_ENV_IDLE) continue;

            switch (v->env_stage) {
                case GRAN_ENV_ATTACK:
                    v->env_level += att_step;
                    if (v->env_level >= 65535U) {
                        v->env_level = 65535U;
                        v->env_stage = GRAN_ENV_DECAY;
                    }
                    break;
                case GRAN_ENV_DECAY:
                    if (v->env_level > sus_target + dec_step) {
                        v->env_level -= dec_step;
                    } else {
                        v->env_level = sus_target;
                        v->env_stage = GRAN_ENV_SUSTAIN;
                    }
                    break;
                case GRAN_ENV_SUSTAIN:
                    v->env_level = sus_target;
                    break;
                case GRAN_ENV_RELEASE:
                    if (v->env_level > rel_step) {
                        v->env_level -= rel_step;
                    } else {
                        v->env_level = 0U;
                        v->env_stage = GRAN_ENV_IDLE;
                        v->active = 0;
                    }
                    break;
                default:
                    v->env_level = 0U;
                    v->active = 0;
                    break;
            }

            if (!v->active) continue;

            v->spawn_counter++;
            if (v->spawn_counter >= v->next_spawn_delay) {
                v->spawn_counter = 0;

                uint32_t timing_jitter = rand_below(&state->rng_state, (base_interval >> 1) + 1U);
                v->next_spawn_delay = ((base_interval * 3U) >> 2) + timing_jitter;

                int32_t jitter_l = (int32_t)rand_below(&state->rng_state, jitter_range * 2U + 1U) - (int32_t)jitter_range;
                int32_t pos_l = (int32_t)base_pos + jitter_l;
                if (pos_l < 0) pos_l = 0;
                if ((uint32_t)pos_l > max_start) pos_l = (int32_t)max_start;
                uint32_t speed_l = randomize_grain_speed(v->current_speed, state->macro_pitch_rnd, &state->rng_state);

                int32_t jitter_r = (int32_t)rand_below(&state->rng_state, jitter_range * 2U + 1U) - (int32_t)jitter_range;
                int32_t pos_r_indep = (int32_t)base_pos + jitter_r;
                if (pos_r_indep < 0) pos_r_indep = 0;
                if ((uint32_t)pos_r_indep > max_start) pos_r_indep = (int32_t)max_start;
                uint32_t speed_r_indep = randomize_grain_speed(v->current_speed, state->macro_pitch_rnd, &state->rng_state);

                uint32_t pos_r = (uint32_t)(pos_l + (((pos_r_indep - pos_l) * (int32_t)stereo_q8) >> 8));
                /* Speeds reach ~2^25 with the +2 octave random range; shift before multiplying. */
                uint32_t speed_r = (uint32_t)((int32_t)speed_l +
                    (((((int32_t)speed_r_indep - (int32_t)speed_l) >> 4) * (int32_t)stereo_q8) >> 4));

                uint32_t rand_offset = ((v->next_spawn_delay * 21846U) >> 16) +
                                       rand_below(&state->rng_state, (v->next_spawn_delay >> 1) + 1U);
                uint32_t r_delay_full = (rand_offset * stereo_q8) >> 8;
                uint16_t r_delay = (uint16_t)(r_delay_full > 65535U ? 65535U : r_delay_full);

                uint8_t pan_l = (uint8_t)(128U - (stereo_q8 >> 1));
                uint8_t pan_r = (uint8_t)(128U + ((stereo_q8 * 127U) >> 8));

                spawn_single_grain(state, v_idx, (uint32_t)pos_l, speed_l, grain_len, win_inc, 0, pan_l);
                spawn_single_grain(state, v_idx, pos_r, speed_r, grain_len, win_inc, r_delay, pan_r);
            }
        }

        int32_t sum_l = 0;
        int32_t sum_r = 0;

        for (int i = 0; i < MAX_GRAINS; i++) {
            Grain *g = &state->grains[i];
            if (!g->active) continue;

            if (g->start_delay > 0) {
                g->start_delay--;
                continue;
            }

            uint32_t pos_int = g->position >> 16;
            if (pos_int >= dlen - 1U) {
                pos_int = dlen - 2U;
            }
            uint32_t pos_frac = g->position & 0xFFFFU;

            int32_t s0 = decoded_ram_buffer[pos_int];
            int32_t s1 = decoded_ram_buffer[pos_int + 1U];
            /* |s1-s0| < 2^17: a Q15 fraction keeps the product inside 32 bits. */
            int32_t sample = s0 + (((s1 - s0) * (int32_t)(pos_frac >> 1)) >> 15);

            uint32_t win_idx = g->win_pos >> 16;
            if (win_idx > 1023U) win_idx = 1023U;
            g->win_pos += g->win_inc;

            int32_t v_env = (int32_t)(state->voices[g->voice_idx].env_level >> 8);
            int32_t windowed = (((sample * hann_window[win_idx]) >> 15) * v_env) >> 8;

            sum_l += (windowed * (int32_t)(255U - g->pan)) >> 8;
            sum_r += (windowed * (int32_t)g->pan) >> 8;

            g->position += g->speed;
            g->counter++;
            if (g->counter >= g->length) g->active = 0;
        }

        /* |sum| < 48 grains * 2^15 < 2^21; pre-shift keeps the product in 32 bits. */
        int32_t mixed_l = ((sum_l >> 4) * norm_q15) >> 11;
        int32_t mixed_r = ((sum_r >> 4) * norm_q15) >> 11;

        // 1. Cascaded Multimode State Variable Filter
        if (state->filter_mode != GRAN_FILT_OFF) {
            for (uint8_t s = 0; s <= filter_stages; s++) {
                int32_t in_l = soft_clip(mixed_l);
                int32_t in_r = soft_clip(mixed_r);
                int32_t q_stage = (s == 0) ? svf_q : svf_q_follower;

                int32_t high_l = svf_limit(in_l - state->svf_low_l[s] - ((q_stage * state->svf_band_l[s]) >> 15));
                state->svf_band_l[s] = svf_limit(state->svf_band_l[s] + ((svf_f * high_l) >> 15));
                state->svf_low_l[s] = svf_limit(state->svf_low_l[s] + ((svf_f * state->svf_band_l[s]) >> 15));

                int32_t high_r = svf_limit(in_r - state->svf_low_r[s] - ((q_stage * state->svf_band_r[s]) >> 15));
                state->svf_band_r[s] = svf_limit(state->svf_band_r[s] + ((svf_f * high_r) >> 15));
                state->svf_low_r[s] = svf_limit(state->svf_low_r[s] + ((svf_f * state->svf_band_r[s]) >> 15));

                if (state->filter_mode == GRAN_FILT_LPF) {
                    mixed_l = state->svf_low_l[s];
                    mixed_r = state->svf_low_r[s];
                } else if (state->filter_mode == GRAN_FILT_BPF) {
                    mixed_l = state->svf_band_l[s];
                    mixed_r = state->svf_band_r[s];
                } else if (state->filter_mode == GRAN_FILT_HPF) {
                    mixed_l = high_l;
                    mixed_r = high_r;
                }
            }
        }

        // 2. Stereo Chorus. The modulation is at most +/-200 frames so both
        //    taps stay inside the 1024-frame line (the previous scaling swept
        //    thousands of frames and wrapped).
        if (state->chorus_depth > 0) {
            uint32_t w = state->chorus_idx;
            state->chorus_l[w] = soft_clip(mixed_l);
            state->chorus_r[w] = soft_clip(mixed_r);

            state->chorus_phase += 2U;
            int32_t tri = (int32_t)(state->chorus_phase & 0xFFFFU);
            if (tri > 32767) tri = 65535 - tri;
            uint32_t mod = ((uint32_t)tri * state->chorus_depth) >> 14;

            uint32_t tap_l = (w + GRAN_CHORUS_LEN - (160U + mod)) & (GRAN_CHORUS_LEN - 1U);
            uint32_t tap_r = (w + GRAN_CHORUS_LEN - (480U - mod)) & (GRAN_CHORUS_LEN - 1U);

            mixed_l += ((int32_t)state->chorus_l[tap_l] * chorus_gain) >> 16;
            mixed_r += ((int32_t)state->chorus_r[tap_r] * chorus_gain) >> 16;
            state->chorus_idx = (w + 1U) & (GRAN_CHORUS_LEN - 1U);
        }

        out_l[f] = soft_clip(mixed_l);
        out_r[f] = soft_clip(mixed_r);
    }
}
