# Granular overlay: hardware safety notes

**Status: never built or flashed on real hardware.** Treat every image from this
branch as untested.

## Before flashing
- Keep the stock V15 firmware (`sdk/firmware/FM-1_015.fwsc`, also on m-vave.com)
  at hand. OTA is not a proven recovery path.
- Flash only an artifact from a CI run where `sdk/Build.ps1` passed all of its
  checks (manifest hashes, SRAM placement of audio code, 1 MiB fit).
- First boot: test with no project you care about; export projects first.

## Design
The sealed SDK knows six source types. Granular is an application overlay:
the SDK runs a PLUCK track at volume 0 (sequencer, recording, mute/solo, MIDI)
and `app/granular_bridge.c` renders granular audio from the same notes.
`GROOVE_GRANULAR` (UI-only, `GROOVE_SOURCES`) is never passed to the SDK.

- Audio callback: `granular_bridge_render()` and `engine_granular.c` DSP only;
  both are SRAM code with no Flash callees (no division helpers, memset, memcpy).
- Control task: everything else (init, parameters, sample decode, SDK calls).
- Only a lock-free 32-event ring and a few volatile flags cross between them.

## Limits
One granular track; not saved in projects (loading a project removes it); no
external-MIDI, chord, gate, probability or arp mirroring; no FX sends on
hardware (reverb/delay in `engine_fx` are browser-only); 8192 source frames
(~0.26 s at 32 kHz) per slot.
