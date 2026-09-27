#pragma once
#include <cstdint>
#include "pipeline_common.h"

// One 40-D log-mel feature frame, quantised to INT8 for the CNN.
struct FeatureFrame {
    int8_t mel[NUM_MEL_BINS];
    float  frame_energy;   // raw (pre-quantisation) energy — reused by VAD, Step 6.7
};

// One-time setup: precompute Hanning window and Mel filterbank matrix.
void dsp_features_init();

// Extracts one feature frame from the most recent FRAME_LEN_SAMPLES of audio
// ending at `write_ptr` in the ring buffer (Steps 2 & 3):
//   framing -> Hanning window -> FFT -> Mel filterbank -> log -> per-band
//   noise subtraction -> INT8 quantisation.
// This is the single computation whose `frame_energy` output feeds BOTH the
// CNN (via mel[]) and the VAD (Step 6.7) — the reason VAD is zero extra cost.
FeatureFrame dsp_features_extract(uint32_t write_ptr);
