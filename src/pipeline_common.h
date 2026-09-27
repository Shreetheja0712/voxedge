#pragma once
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- Audio format (Step 1) ----
constexpr uint32_t SAMPLE_RATE_HZ   = 16000;
constexpr uint32_t BITS_PER_SAMPLE  = 16;
constexpr uint32_t BYTES_PER_SAMPLE = BITS_PER_SAMPLE / 8;

// ---- Ring buffer (Step 1) : 32 KB = 1.0 s of rolling history ----
constexpr uint32_t RING_BUFFER_BYTES = 32 * 1024;

// ---- Framing (Step 2) ----
constexpr uint32_t FRAME_LEN_SAMPLES = 480;   // 30 ms @ 16 kHz
constexpr uint32_t HOP_LEN_SAMPLES   = 320;   // 20 ms @ 16 kHz -> 50 frames/sec
constexpr uint32_t HOP_LEN_BYTES     = HOP_LEN_SAMPLES * BYTES_PER_SAMPLE;

// ---- Mel / feature extraction (Step 3) ----
constexpr uint32_t FFT_SIZE       = 512;      // next pow2 >= FRAME_LEN_SAMPLES
constexpr uint32_t NUM_MEL_BINS   = 40;

// ---- KWS temporal logic (Step 4) ----
constexpr uint32_t TEMPORAL_WINDOW_FRAMES = 30;   // 600 ms moving average
constexpr float     TRIGGER_THRESHOLD     = 0.70f;

// ---- VAD (Step 6.7) - reuses frame_energy, NOT a separate model ----
constexpr uint32_t VAD_SILENCE_FRAMES = 40;   // 40 * 20ms = 800 ms

// Shared ring buffer object. DMA/audio task is the sole writer;
// KWS task and Net task are readers (never touch write_ptr).
struct RingBuffer {
    uint8_t  data[RING_BUFFER_BYTES];
    volatile uint32_t write_ptr = 0;   // bytes, wraps at RING_BUFFER_BYTES
};

// System state machine (Step 7.4 re-arm)
enum class SystemState : uint8_t {
    IDLE_KWS = 0,
    STREAMING,
    AWAITING_RESPONSE,
};

// Cross-task handles (defined in app_main.cpp)
extern RingBuffer      g_ring_buf;
extern TaskHandle_t    g_net_task_handle;
extern volatile SystemState g_system_state;

// Notification value sent Core0 -> Core1 on trigger: carries read_ptr (Step 5/6)
struct TriggerMsg {
    uint32_t read_ptr_bytes;
};
extern volatile TriggerMsg g_trigger_msg;

// Notification bits used with xTaskNotify (Core1 net task)
constexpr uint32_t NOTIFY_BIT_TRIGGER      = (1 << 0);  // KWS -> Net: start streaming
constexpr uint32_t NOTIFY_BIT_END_OF_UTT   = (1 << 1);  // VAD  -> Net: 800ms silence
