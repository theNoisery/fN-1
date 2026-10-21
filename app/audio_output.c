/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "instrument.h"
#include "synth/engine_granular.h"

static unsigned output_gain;
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));

GranularVoiceState granular_state;
static int granular_is_init = 0;

void fm1_output_reset(void) { 
    output_gain = 0; 
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
    
    // Read the physical keyboard state
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (groove_source_type(t) == GROOVE_GRANULAR) {
            granular_active = 1;
            struct groove_view view;
            groove_get_view(t, 0, &view);
            
            // Gate the audio: Only open the volume if a key is currently held down
            if (controls.keys > 0) {
                gran_vol = view.volume; 
            }
            break;
        }
    }
    
    if (granular_active) {
        granular_render_block(&granular_state, gran_l, gran_r, FM1_BLOCK_FRAMES);
    }

    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int64_t value = ((int64_t)output[i] * output_gain) >> 7;
        if (usb_frames == FM1_BLOCK_FRAMES)
            value += ((int64_t)usb_pcm[i] * ((i & 1U) ? right : left)) >> 7;
            
        // Splice the grains into the final output mix, scaled by our key gate
        if (granular_active && gran_vol > 0) {
            int16_t gran_sample = (i & 1U) ? gran_r[i >> 1] : gran_l[i >> 1];
            value += ((int64_t)gran_sample * gran_vol * output_gain) >> 13; 
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
