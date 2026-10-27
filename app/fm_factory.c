/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm_patch.h"
#include "fm_factory.inc"

/* Size-limited build: only the first 16 V15 factory voices are built in; the
 * other programs repeat them until a user bank is imported (user banks take
 * priority). The table is const FM1_DSP_DATA, which lives in SRAM, so it is
 * safe to read from audio code while Flash is being programmed. */
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    return factory_raw+(program%16U)*128;
}
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program)
{
    groove_fm_unpack(p,groove_fm_factory_raw(program));
}
