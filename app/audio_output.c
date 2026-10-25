/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "instrument.h"
#include "synth/engine_granular.h"
#include <string.h>

extern unsigned groove_screen_active_track(void);
extern int groove_screen_octave(void);

static unsigned output_gain;
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));

GranularVoiceState granular_state;
static int granular_is_init = 0;

// Shared send buses to feed the Granular track into the Global FX engine (effects.c)
int32_t gran_fx_rev_bus[FM1_BLOCK_FRAMES];
int32_t gran_fx_dly_bus[FM1_BLOCK_FRAMES];

static uint8_t key_down_state[27];
static uint8_t last_seq_note = 255;

void fm1_output_reset(void) { 
    output_gain = 0;
    last_seq_note = 255;
    memset(key_down_state, 0, sizeof(key_down_state));
    memset(gran_fx_rev_bus, 0, sizeof(gran_fx_rev_bus));
    memset(gran_fx_dly_bus, 0, sizeof(gran_fx_dly_bus));
    granular_init(&granular_state, 32000); 
    granular_is_init = 1;
}

static inline __attribute__((always_inline)) int32_t limit(int64_t value)
{
    if (((uint64_t)value + 8388608U) >> 24)
        return value < 0 ? -8388608 : 8388607;
    return (int32_t)value;
}

FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    
    if (!granular_is_init) fm1_output_reset();

    unsigned target = fm1_engine_output_level();
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));

    uint16_t usb_gain[2] = {0, 0};
    int usb_frames = fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, usb_gain);
    unsigned left = ((unsigned)usb_gain[0] * output_gain) >> 7;
    unsigned right = ((unsigned)usb_gain[1] * output_gain) >> 7;
    
    int16_t gran_l[FM1_BLOCK_FRAMES];
    int16_t gran_r[FM1_BLOCK_FRAMES];
    
    int granular_active = 0;
    unsigned gran_vol = 0;
    uint8_t gran_sends[2] = {0, 0};
    
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    unsigned active_track = groove_screen_active_track();
    
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (groove_source_type(t) == GROOVE_GRANULAR) {
            granular_active = 1;
            struct groove_view view;
            groove_get_view(t, 0, &view);
            
            gran_vol = view.muted ? 0 : granular_state.volume;
            groove_sends_get(t, gran_sends);
            
            int can_play_keys = (t == active_track) || (groove_track_role(t) == GROOVE_TRACK_PLAY);
            uint32_t current_keys = can_play_keys ? (controls.keys & 0x7FFFFFFU) : 0U;

            // 1. Independent Polyphonic Key State Evaluator
            int any_key_held = 0;
            int oct = groove_screen_octave();
            for (unsigned k = 0; k < 27; k++) {
                int is_pressed = (current_keys & (1U << k)) != 0;
                unsigned note = groove_key_note(t, k, oct);

                if (is_pressed) {
                    any_key_held = 1;
                    if (!key_down_state[k]) {
                        key_down_state[k] = 1;
                        if (note < 128) granular_note_on(&granular_state, (uint8_t)note, 100);
                    }
                } else {
                    if (key_down_state[k]) {
                        key_down_state[k] = 0;
                        if (note < 128) granular_note_off(&granular_state, (uint8_t)note);
                    }
                }
            }
            
            // 2. Sequencer Playback (when no live keys are held)
            if (!any_key_held) {
                if (view.playing) {
                    if (view.step.velocity > 0 && view.step.note != last_seq_note) {
                        if (last_seq_note < 128) granular_note_off(&granular_state, last_seq_note);
                        granular_note_on(&granular_state, view.step.note, view.step.velocity);
                        last_seq_note = view.step.note;
                    } else if (view.step.velocity == 0 && last_seq_note != 255) {
                        granular_note_off(&granular_state, last_seq_note);
                        last_seq_note = 255;
                    }
                } else if (last_seq_note != 255) {
                    granular_note_off(&granular_state, last_seq_note);
                    last_seq_note = 255;
                }
            }
            break;
        }
    }
    
    if (granular_active) {
        granular_render_block(&granular_state, gran_l, gran_r, FM1_BLOCK_FRAMES);
        // Feed the Granular track's REV SEND and DLY SEND into the Global FX bus
        for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f) {
            int32_t mono = (((int32_t)gran_l[f] + (int32_t)gran_r[f]) * (int32_t)gran_vol) / 200;
            gran_fx_rev_bus[f] = (mono * (int32_t)gran_sends[0] * 512) / 15;
            gran_fx_dly_bus[f] = (mono * (int32_t)gran_sends[1] * 512) / 15;
        }
    } else {
        memset(gran_fx_rev_bus, 0, sizeof(gran_fx_rev_bus));
        memset(gran_fx_dly_bus, 0, sizeof(gran_fx_dly_bus));
    }

    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int64_t value = ((int64_t)output[i] * output_gain) >> 7;
        if (usb_frames == FM1_BLOCK_FRAMES)
            value += ((int64_t)usb_pcm[i] * ((i & 1U) ? right : left)) >> 7;
            
        if (granular_active && gran_vol > 0) {
            int16_t gran_sample = (i & 1U) ? gran_r[i >> 1] : gran_l[i >> 1];
            value += ((int64_t)gran_sample * gran_vol * output_gain) >> 6; 
        }
            
        output[i] = usb_frames == -2 ? 0 : limit(value);
    }
    
    if (fm1_bluetooth_audio_read(bluetooth_pcm, FM1_BLOCK_FRAMES) == FM1_BLOCK_FRAMES) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t value = output[i] + (int32_t)bluetooth_pcm[i] * (int32_t)(output_gain * 2U);
            output[i] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
        }
    }
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
}
