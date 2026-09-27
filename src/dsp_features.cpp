#include "dsp_features.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include "esp_log.h"

// Requires the "espressif/esp-dsp" component (idf.py add-dependency
// espressif/esp-dsp) for dsps_fft2r_fc32 / dsps_wind_hann_f32.
#include "esp_dsp.h"

// mel_filterbank.h is generated OFFLINE (e.g. with librosa) and checked
// into the repo as a constant array: mel_fb[NUM_MEL_BINS][FFT_SIZE/2 + 1].
// Not regenerated on-device — this keeps the ESP32 firmware free of any
// floating-point filterbank-design code.
#include "mel_filterbank.h"

static const char* TAG = "dsp_features";

static float s_hann_window[FRAME_LEN_SAMPLES];
static float s_noise_floor[NUM_MEL_BINS];   // running per-band noise estimate
static bool  s_noise_floor_init = false;

void dsp_features_init()
{
    dsps_wind_hann_f32(s_hann_window, FRAME_LEN_SAMPLES);
    std::fill(std::begin(s_noise_floor), std::end(s_noise_floor), -80.0f); // dBish floor
    ESP_LOGI(TAG, "DSP init done: %lu-pt FFT, %u mel bins",
             (unsigned long)FFT_SIZE, NUM_MEL_BINS);
}

FeatureFrame dsp_features_extract(uint32_t write_ptr)
{
    FeatureFrame out{};

    // --- Step 2: pull the most recent FRAME_LEN_SAMPLES out of the ring
    //     buffer (handles wrap-around) and apply the Hanning window ---
    static float windowed[FFT_SIZE];  // zero-padded to FFT_SIZE
    std::fill(std::begin(windowed), std::end(windowed), 0.0f);

    uint32_t frame_bytes = FRAME_LEN_SAMPLES * BYTES_PER_SAMPLE;
    uint32_t start = (write_ptr + RING_BUFFER_BYTES - frame_bytes) % RING_BUFFER_BYTES;

    for (uint32_t i = 0; i < FRAME_LEN_SAMPLES; ++i) {
        uint32_t byte_off = (start + i * BYTES_PER_SAMPLE) % RING_BUFFER_BYTES;
        int16_t sample;
        memcpy(&sample, &g_ring_buf.data[byte_off], sizeof(int16_t));
        windowed[i] = (static_cast<float>(sample) / 32768.0f) * s_hann_window[i];
    }

    // --- Step 3a: FFT (esp-dsp radix-2, in-place complex) ---
    static float fft_buf[FFT_SIZE * 2];  // interleaved re/im
    for (uint32_t i = 0; i < FFT_SIZE; ++i) {
        fft_buf[2 * i]     = windowed[i];
        fft_buf[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(fft_buf, FFT_SIZE);
    dsps_bit_rev2r_fc32(fft_buf, FFT_SIZE);

    static float power_spec[FFT_SIZE / 2 + 1];
    for (uint32_t k = 0; k <= FFT_SIZE / 2; ++k) {
        float re = fft_buf[2 * k];
        float im = fft_buf[2 * k + 1];
        power_spec[k] = re * re + im * im;
    }

    // --- Step 3b: Mel filterbank + log + per-band noise subtraction ---
    float total_energy = 0.0f;
    for (uint32_t m = 0; m < NUM_MEL_BINS; ++m) {
        float band_sum = 0.0f;
        for (uint32_t k = 0; k <= FFT_SIZE / 2; ++k) {
            band_sum += power_spec[k] * mel_fb[m][k];
        }
        float log_mel = 10.0f * log10f(band_sum + 1e-8f);

        // Slow-moving noise floor estimate, updated only when this band is
        // quiet, so speech doesn't drag the floor upward.
        if (!s_noise_floor_init) {
            s_noise_floor[m] = log_mel;
        } else if (log_mel < s_noise_floor[m] + 3.0f) {
            s_noise_floor[m] = 0.95f * s_noise_floor[m] + 0.05f * log_mel;
        }
        float denoised = std::max(log_mel - s_noise_floor[m], 0.0f);
        total_energy += denoised;

        // INT8 quantisation for the CNN (Step 4) — scale is model-specific;
        // this linear map matches the QAT range used at training time.
        constexpr float QUANT_SCALE = 127.0f / 60.0f;  // expects ~0..60 dB range
        int q = static_cast<int>(denoised * QUANT_SCALE) - 128;
        out.mel[m] = static_cast<int8_t>(std::clamp(q, -128, 127));
    }
    s_noise_floor_init = true;

    out.frame_energy = total_energy;  // <-- reused directly by VAD, Step 6.7
    return out;
}
