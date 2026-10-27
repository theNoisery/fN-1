/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "granular_bridge.h"
#include "synth/engine_granular.h"
#include "engine.h" /* FM1_DSP_CODE */

GranularVoiceState granular_state;
int32_t gran_fx_rev_bus[FM1_BLOCK_FRAMES];
int32_t gran_fx_dly_bus[FM1_BLOCK_FRAMES];
volatile uint8_t gran_reset_request;

/* ---- shared state ------------------------------------------------------- */

enum { EV_NOTE_ON = 1, EV_NOTE_OFF = 2 };
enum { RING = 32 }; /* power of two */

static struct { uint8_t type, note, velocity, unused; } ring[RING];
/* Single producer (control task), single consumer (audio callback). Each side
 * writes only its own index; the producer publishes an event by writing the
 * slot first and the head last. */
static volatile uint32_t ring_head, ring_tail;
static volatile uint8_t ring_overflow;  /* an event was dropped: release all notes */
static volatile uint8_t gran_enabled;   /* a granular track exists */
static volatile uint8_t gran_audible;   /* not muted / soloed away */
static volatile uint8_t gran_rev_send, gran_dly_send;

/* ---- control task ------------------------------------------------------- */

static uint8_t virtual_mask;     /* bit per track; at most one bit is ever set */
static uint8_t saved_volume;     /* the backing track's SDK volume before muting it */
static uint8_t seq_note = 255;   /* note started by the sequencer, 255 = none */
static uint8_t seq_step, seq_running;
static uint8_t initialized;

static void push(uint8_t type, uint8_t note, uint8_t velocity)
{
    uint32_t head = ring_head, next = (head + 1U) & (RING - 1U);
    if (next == ring_tail) {
        ring_overflow = 1;
        return;
    }
    ring[head].type = type;
    ring[head].note = note;
    ring[head].velocity = velocity;
    GRAN_NO_IDIOM(); /* slot contents before head */
    ring_head = next;
}

void granular_bridge_init(void)
{
    if (initialized) return;
    initialized = 1;
    gran_enabled = 0;
    granular_init(&granular_state, GRANULAR_OUT_RATE);
    gran_reset_request = 1;
}

unsigned granular_bridge_virtual(unsigned track)
{
    return track < GROOVE_TRACKS && ((virtual_mask >> track) & 1U) &&
           groove_source_type(track) == GROOVE_PLUCK &&
           groove_track_role(track) != GROOVE_TRACK_OFF;
}

static unsigned active_track(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (granular_bridge_virtual(t)) return t;
    return GROOVE_TRACKS;
}

int granular_bridge_assign(unsigned track)
{
    if (track >= GROOVE_TRACKS) return GRANULAR_BRIDGE_ERROR;
    unsigned other = active_track();
    if (other < GROOVE_TRACKS && other != track) return GRANULAR_BRIDGE_BUSY;
    if (groove_source_set(track, GROOVE_PLUCK)) return GRANULAR_BRIDGE_ERROR;
    struct groove_view view = {0};
    groove_get_view(track, 0, &view);
    if (!((virtual_mask >> track) & 1U)) saved_volume = view.volume ? view.volume : 100;
    /* Mixer settings survive a source change, so this keeps the PLUCK silent.
     * groove_control_edit saturates at 0. */
    if (view.volume) groove_control_edit(track, 0, -(int)view.volume, 0);
    virtual_mask = (uint8_t)(1U << track);
    seq_note = 255;
    seq_running = 0;
    return GRANULAR_BRIDGE_OK;
}

void granular_bridge_release(unsigned track)
{
    if (track >= GROOVE_TRACKS || !((virtual_mask >> track) & 1U)) return;
    virtual_mask = 0;
    seq_note = 255;
    seq_running = 0;
    /* Hand the track back with an audible volume; the new source inherits the
     * muted PLUCK's volume otherwise. */
    struct groove_view view = {0};
    groove_get_view(track, 0, &view);
    int restore = (int)(saved_volume ? saved_volume : 100) - (int)view.volume;
    if (restore) groove_control_edit(track, 0, restore, 0);
}

void granular_bridge_forget(void)
{
    virtual_mask = 0;
    seq_note = 255;
    seq_running = 0;
    gran_enabled = 0; /* the audio side drops events and silences voices */
}

void granular_bridge_note_on(unsigned note, unsigned velocity)
{
    if (note < 128U) push(EV_NOTE_ON, (uint8_t)note, (uint8_t)(velocity > 127U ? 127U : velocity));
}

void granular_bridge_note_off(unsigned note)
{
    if (note < 128U) push(EV_NOTE_OFF, (uint8_t)note, 0);
}

void granular_bridge_control(void)
{
    unsigned track = active_track();
    if (track >= GROOVE_TRACKS) {
        virtual_mask = 0; /* the backing source was replaced behind our back */
        gran_enabled = 0;
        seq_note = 255;
        seq_running = 0;
        return;
    }
    struct groove_view view = {0};
    groove_get_view(track, 0, &view);
    uint8_t sends[2] = {0, 0};
    groove_sends_get(track, sends);
    gran_audible = view.audible ? 1U : 0U;
    gran_rev_send = sends[0];
    gran_dly_send = sends[1];
    gran_enabled = 1;

    /* Follow the SDK sequencer. A step is (re)triggered when the play position
     * changes. Chords, gate length, ties, probability and the arpeggiator are
     * the SDK's and are not mirrored: the root note of each step is played
     * until the next step. */
    if (view.playing) {
        if (!seq_running || view.play_step != seq_step) {
            struct groove_view cell = {0};
            groove_get_view(track, view.play_step, &cell);
            seq_step = view.play_step;
            seq_running = 1;
            if (seq_note < 128U) granular_bridge_note_off(seq_note);
            seq_note = 255;
            if (cell.step.velocity && cell.step.note < 128U) {
                granular_bridge_note_on(cell.step.note, cell.step.velocity);
                seq_note = cell.step.note;
            }
        }
    } else if (seq_running) {
        if (seq_note < 128U) granular_bridge_note_off(seq_note);
        seq_note = 255;
        seq_running = 0;
    }
}

/* ---- audio callback (SRAM; no Flash callees) ---------------------------- */

static int16_t pcm_left[FM1_BLOCK_FRAMES];
static int16_t pcm_right[FM1_BLOCK_FRAMES];
static int32_t mix_q23[FM1_BLOCK_WORDS];
static uint8_t was_enabled;
#ifdef FM1_OPEN_WEB
static uint8_t buses_dirty;
#endif

static FM1_DSP_CODE void drop_events(void)
{
    ring_tail = ring_head;
    ring_overflow = 0;
}

#ifdef FM1_OPEN_WEB
static FM1_DSP_CODE void clear_buses(void)
{
    if (!buses_dirty) return;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        gran_fx_rev_bus[i] = 0;
        gran_fx_dly_bus[i] = 0;
        GRAN_NO_IDIOM();
    }
    buses_dirty = 0;
}
#endif

FM1_AUDIO_CODE const int32_t *granular_bridge_render(void)
{
    if (gran_reset_request) {
        gran_reset_request = 0;
        drop_events();
        granular_silence(&granular_state);
        was_enabled = 0;
    }
    if (!gran_enabled) {
        drop_events();
        if (was_enabled) {
            granular_silence(&granular_state);
            was_enabled = 0;
        }
#ifdef FM1_OPEN_WEB
        clear_buses();
#endif
        return (const int32_t *)0;
    }
    was_enabled = 1;

    if (ring_overflow) {
        ring_overflow = 0;
        granular_all_notes_off(&granular_state);
    }
    uint32_t tail = ring_tail;
    const uint32_t head = ring_head;
    while (tail != head) {
        if (ring[tail].type == EV_NOTE_ON)
            granular_note_on(&granular_state, ring[tail].note, ring[tail].velocity);
        else if (ring[tail].type == EV_NOTE_OFF)
            granular_note_off(&granular_state, ring[tail].note);
        tail = (tail + 1U) & (RING - 1U);
    }
    ring_tail = tail;

    unsigned volume = gran_audible ? granular_state.volume : 0U;
    if (volume > 100U) volume = 100U;
    if (!volume || !granular_is_ready()) {
#ifdef FM1_OPEN_WEB
        clear_buses();
#endif
        return (const int32_t *)0;
    }

    granular_render_block(&granular_state, pcm_left, pcm_right, FM1_BLOCK_FRAMES);

    /* int16 -> Q23 (x256) scaled by volume/100: x * volume * 655 / 256.
     * |x| * 100 * 655 < 2^31, so no 64-bit product or divide is needed. */
    const int32_t scale = (int32_t)volume * 655;
    for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f) {
        mix_q23[2U * f] = ((int32_t)pcm_left[f] * scale) >> 8;
        mix_q23[2U * f + 1U] = ((int32_t)pcm_right[f] * scale) >> 8;
    }
#ifdef FM1_OPEN_WEB
    /* Mono send into the browser reverb/delay (Q24 bus). 512/15 ~ 34. */
    const int32_t rev = (int32_t)gran_rev_send * 34, dly = (int32_t)gran_dly_send * 34;
    for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f) {
        int32_t mono = (((int32_t)pcm_left[f] + (int32_t)pcm_right[f]) >> 1) * (int32_t)volume;
        mono = (mono * 655) >> 16; /* x * volume / 100 */
        gran_fx_rev_bus[f] = mono * rev;
        gran_fx_dly_bus[f] = mono * dly;
    }
    buses_dirty = 1;
#endif
    return mix_q23;
}
