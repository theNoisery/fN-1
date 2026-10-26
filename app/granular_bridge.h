/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_GRANULAR_BRIDGE_H
#define FM1_GRANULAR_BRIDGE_H
#include <stdint.h>
#include "fm1.h"
#include "instrument.h"
#include "synth/engine_granular.h"

/* The sealed SDK knows six source types (DX7 ... SAMPLER) and cannot be given
 * a seventh. Granular is therefore an application-layer overlay: the SDK runs
 * an ordinary PLUCK track at volume 0 (it keeps the sequencer, recording,
 * mute/solo and MIDI channel), and this bridge renders the granular engine
 * from the same notes. GROOVE_GRANULAR must never be passed to an SDK call.
 *
 * Threading contract
 *  - Control task: everything declared under "control task". Flash is fine.
 *  - Audio callback: granular_bridge_render() only. It is SRAM code and may
 *    call nothing that lives in Flash (no SDK control calls, no memset/memcpy,
 *    no 32-bit divide helpers, no panel/ADC reads).
 *  - The two sides share only the volatile flags and the event ring below. */

/* UI-only source id, one past the SDK's last. It is never stored in a project
 * and never passed to groove_source_set(); the SDK enum (GROOVE_SOURCES)
 * stays at its stock value. */
#define GROOVE_GRANULAR ((unsigned)GROOVE_SOURCES)
#define UI_SOURCES (GROOVE_SOURCES + 1U)

enum {
    GRANULAR_BRIDGE_OK = 0,
    GRANULAR_BRIDGE_ERROR = -1, /* the SDK refused the backing source */
    GRANULAR_BRIDGE_BUSY = -2   /* another track already carries the engine */
};

extern GranularVoiceState granular_state;   /* zero state is silent */
extern int32_t gran_fx_rev_bus[FM1_BLOCK_FRAMES]; /* browser FX sends only */
extern int32_t gran_fx_dly_bus[FM1_BLOCK_FRAMES];
extern volatile uint8_t gran_reset_request;

/* fm1_output_reset() may be called from any context: it only sets a flag. */
static inline void granular_bridge_request_reset(void) { gran_reset_request = 1; }

/* ---- control task ---- */
void granular_bridge_init(void);
/* Nonzero when `track` currently carries the granular engine. */
unsigned granular_bridge_virtual(unsigned track);
/* Make `track` the granular track (backed by a silent PLUCK source). */
int granular_bridge_assign(unsigned track);
/* Give `track` back to the SDK after its source was changed (restores the
 * backing track's volume). Safe to call for a track that is not granular. */
void granular_bridge_release(unsigned track);
/* Drop the overlay without touching the SDK (after a project/preset load has
 * replaced every source and mixer setting). */
void granular_bridge_forget(void);
void granular_bridge_note_on(unsigned note, unsigned velocity);
void granular_bridge_note_off(unsigned note);
/* Once per control tick: publishes mute/sends and follows the sequencer. */
void granular_bridge_control(void);

/* ---- audio callback ---- */
/* Renders one block. Returns interleaved signed Q23 stereo scaled by the
 * track volume (before master gain), or NULL when granular is silent. */
FM1_AUDIO_CODE const int32_t *granular_bridge_render(void);
#endif
