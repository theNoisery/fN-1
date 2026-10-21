#pragma once
#include <stdint.h>
#include <stddef.h>

#define MAX_GRAINS 8

typedef struct {
    int32_t position;    // Fixed-point read pointer (Q16.16)
    int32_t speed;       // Pitch/playback rate multiplier (Q16.16)
    int16_t length;      // Total grain length in samples
    int16_t counter;     // Current playback position inside the grain
    int16_t env_pos;     // Envelope pointer for windowing lookup
    uint8_t active;      // 1 if grain is playing, 0 if idle
} Grain;

typedef struct {
    uint32_t sample_rate;
    const int16_t *sample_buffer; // Pointer to shared .fm1s sample slot RAM
    uint32_t sample_length;       // Total length of loaded sample
    
    // OP-1 Macro Knobs (Mapped K1 - K4)
    uint16_t macro_scan;     // K1: Sample read position / Jitter
    uint16_t macro_size;     // K2: Grain duration (20ms to 250ms)
    uint16_t macro_density;  // K3: Spawn rate / Overlap count
    uint16_t macro_spray;    // K4: Pitch scatter / Random detune
    
    uint32_t spawn_counter;
    Grain grains[MAX_GRAINS];
} GranularVoiceState;
