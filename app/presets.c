/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"

/* Size-limited build: the sixteen generated factory demo projects are removed
 * to make room in the 1 MiB image. The preset browser then shows no factory
 * entries; saved project slots are unaffected. */
const char *groove_factory_name(unsigned index)
{
    (void)index;
    return NULL;
}
int groove_factory_build(unsigned index, struct groove_project *p)
{
    (void)index;
    (void)p;
    return GROOVE_PROJECT_INVALID;
}
