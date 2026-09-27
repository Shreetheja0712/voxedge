#pragma once
#include "dsp_features.h"

// Zero-cost VAD (Step 6.7). Deliberately NOT a model: it consumes the
// frame_energy value dsp_features_extract() already computed for the CNN,
// so this adds one comparison + one counter per frame, no extra SRAM.
//
// Call once per frame, only while SYSTEM_STATE == STREAMING. Returns true
// exactly once, on the frame where 800 ms of continuous silence completes.
bool vad_push_frame(float frame_energy);

// Resets the silence counter and noise floor tracking (Step 7.4 re-arm).
void vad_reset();
