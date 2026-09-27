#include "pipeline_common.h"
#include "audio_capture.h"
#include "dsp_features.h"
#include "kws_model.h"
#include "vad.h"
#include "net_task.h"
#include "esp_log.h"

static const char* TAG = "app_main";

// ---- Cross-task globals (declared extern in pipeline_common.h) ----
RingBuffer   g_ring_buf;
TaskHandle_t g_net_task_handle = nullptr;
volatile SystemState g_system_state = SystemState::IDLE_KWS;
volatile TriggerMsg  g_trigger_msg{0};

// Rolling history of (frame_energy, write_ptr) used only for Step 6.2's
// backward scan — needs to cover the CNN's ~620ms receptive field + 600ms
// smoothing window, so we keep the same STACK_FRAMES-ish depth (50 frames
// = 1.0s), matching the ring buffer's own 1.0s capacity.
constexpr uint32_t ENERGY_HISTORY_LEN = 50;
static float    s_energy_hist[ENERGY_HISTORY_LEN] = {0};
static uint32_t s_ptr_hist[ENERGY_HISTORY_LEN]     = {0};
static uint32_t s_hist_idx = 0;

// Step 6.2 — scan backward from "now" for the silence minimum between the
// wake word and the command, then find where energy rises again.
static uint32_t compute_preroll_read_ptr()
{
    // Iterate in chronological order: oldest is at s_hist_idx,
    // newest is at (s_hist_idx - 1) % ENERGY_HISTORY_LEN.
    // Find the absolute energy minimum (silence between wake word and command),
    // then walk forward to where energy rises — that's the command start.
    uint32_t min_chron = 0;
    float min_val = s_energy_hist[s_hist_idx % ENERGY_HISTORY_LEN];
    for (uint32_t c = 0; c < ENERGY_HISTORY_LEN; ++c) {
        uint32_t idx = (s_hist_idx + c) % ENERGY_HISTORY_LEN;
        if (s_energy_hist[idx] < min_val) {
            min_val = s_energy_hist[idx];
            min_chron = c;
        }
    }
    // Walk forward (in time) from the minimum until energy rises meaningfully.
    uint32_t rise_chron = min_chron;
    for (uint32_t c = min_chron; c < ENERGY_HISTORY_LEN; ++c) {
        uint32_t idx = (s_hist_idx + c) % ENERGY_HISTORY_LEN;
        if (s_energy_hist[idx] > min_val * 1.5f + 1.0f) {
            rise_chron = c;
            break;
        }
    }
    uint32_t rise_idx = (s_hist_idx + rise_chron) % ENERGY_HISTORY_LEN;
    return s_ptr_hist[rise_idx];
}

// ---------------------------------------------------------------------
// Core 0 — KWS_Task: Steps 1-4, plus VAD (6.7) once streaming has begun.
// Never calls a network function; never blocks on Core 1.
// ---------------------------------------------------------------------
static void kws_task(void* /*pv*/)
{
    audio_capture_init();
    dsp_features_init();
    kws_model_init();

    for (;;) {
        uint32_t wp = audio_capture_read_next_hop();          // Step 1
        FeatureFrame f = dsp_features_extract(wp);             // Steps 2 & 3

        s_energy_hist[s_hist_idx] = f.frame_energy;
        s_ptr_hist[s_hist_idx] = wp;
        s_hist_idx = (s_hist_idx + 1) % ENERGY_HISTORY_LEN;

        if (g_system_state == SystemState::IDLE_KWS) {
            bool triggered = kws_model_push_frame(f);           // Step 4
            if (triggered) {
                g_trigger_msg.read_ptr_bytes = compute_preroll_read_ptr(); // 6.2
                if (g_net_task_handle) {
                    xTaskNotify(g_net_task_handle, NOTIFY_BIT_TRIGGER, eSetBits);
                }
                ESP_LOGI(TAG, "TRIGGER FIRED — signalled Core 1");
            }
        } else if (g_system_state == SystemState::STREAMING) {
            // Notify Core 1 that a new hop is available for live streaming
            if (g_net_task_handle) {
                xTaskNotify(g_net_task_handle, NOTIFY_BIT_TRIGGER, eSetBits);
            }
            if (vad_push_frame(f.frame_energy) && g_net_task_handle) {  // Step 6.7
                xTaskNotify(g_net_task_handle, NOTIFY_BIT_END_OF_UTT, eSetBits);
            }
        }
        // AWAITING_RESPONSE: KWS keeps capturing/extracting (cheap), just
        // doesn't run inference or VAD — nothing to do here either way.
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "VoxEdge boot: KWS on Core 0 first, Wi-Fi/Net on Core 1 after");

    // Core 0: KWS task, HIGH priority, spawned first so listening begins
    // before Wi-Fi/network init even starts (Step 5.1 decoupled boot).
    xTaskCreatePinnedToCore(kws_task, "kws_task", 8192, nullptr,
                             configMAX_PRIORITIES - 1, nullptr, 0);

    // Core 1: Net task, LOWER priority.
    xTaskCreatePinnedToCore(net_task, "net_task", 8192, nullptr,
                             tskIDLE_PRIORITY + 1, &g_net_task_handle, 1);
}
