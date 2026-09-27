#include "kws_model.h"
#include <cstring>
#include <algorithm>
#include "esp_log.h"

// Requires the "espressif/esp-tflite-micro" component.
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

// model_weights.h is produced OFFLINE by tools/train_and_convert.py:
//   TF/Keras -> QAT -> TFLite INT8 -> xxd -> const unsigned char array.
#include "model_weights.h"

static const char* TAG = "kws_model";

constexpr uint32_t TENSOR_ARENA_BYTES = 32 * 1024;  // matches Resource Budget
static uint8_t s_tensor_arena[TENSOR_ARENA_BYTES];

static const tflite::Model* s_model = nullptr;
static tflite::MicroInterpreter* s_interpreter = nullptr;
static TfLiteTensor* s_input = nullptr;
static TfLiteTensor* s_output = nullptr;

// Rolling [50 x 40] feature stack (Step 3->4 accumulation): 50 frames of
// 20 ms hop = 1.0 s context window feeding the DS-CNN.
constexpr uint32_t STACK_FRAMES = 50;
static int8_t s_feature_stack[STACK_FRAMES][NUM_MEL_BINS];
static uint32_t s_stack_fill = 0;

// Temporal smoothing / peak detection state (Step 4.4 / 4.5)
static float s_recent_scores[TEMPORAL_WINDOW_FRAMES] = {0};
static uint32_t s_score_idx = 0;

// Three-state peak detector per pipeline §4.5:
//   IDLE → TRACKING (score > 0.70) → FIRE (score drops < 0.60)
enum class PeakState : uint8_t { IDLE, TRACKING };
static PeakState s_peak_state = PeakState::IDLE;
static float s_peak_value = 0.0f;
constexpr float TRIGGER_EXIT_THRESHOLD = 0.60f;

void kws_model_init()
{
    s_model = tflite::GetModel(g_model_weights_tflite);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema mismatch");
        return;
    }

    static tflite::MicroMutableOpResolver<6> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddMean();        // GlobalAvgPool2D per the architecture note
    resolver.AddReshape();

    static tflite::MicroInterpreter interpreter(
        s_model, resolver, s_tensor_arena, TENSOR_ARENA_BYTES);
    s_interpreter = &interpreter;

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed — check TENSOR_ARENA_BYTES");
        return;
    }
    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    ESP_LOGI(TAG, "KWS model ready, arena used: %u bytes",
             (unsigned)s_interpreter->arena_used_bytes());
}

static bool run_inference_and_smooth()
{
    // Copy the [50x40] INT8 stack into the model's input tensor.
    memcpy(s_input->data.int8, s_feature_stack, sizeof(s_feature_stack));

    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "inference failed");
        return false;
    }

    // 3-class softmax output (dequantise if needed depending on your
    // output tensor's quantisation params).
    float p_wake = (s_output->data.int8[static_cast<int>(KwsClass::WAKE_WORD)]
                     - s_output->params.zero_point) * s_output->params.scale;

    // 30-frame moving average
    s_recent_scores[s_score_idx] = p_wake;
    s_score_idx = (s_score_idx + 1) % TEMPORAL_WINDOW_FRAMES;
    float avg = 0.0f;
    for (float v : s_recent_scores) avg += v;
    avg /= TEMPORAL_WINDOW_FRAMES;

    // Peak detector (Step 4.5):
    //   IDLE      + avg > 0.70  →  TRACKING, record peak
    //   TRACKING  + avg > peak  →  update peak (still rising, wait)
    //   TRACKING  + avg < 0.60  →  FIRE TRIGGER, back to IDLE
    bool fired = false;
    switch (s_peak_state) {
        case PeakState::IDLE:
            if (avg > TRIGGER_THRESHOLD) {
                s_peak_state = PeakState::TRACKING;
                s_peak_value = avg;
            }
            break;
        case PeakState::TRACKING:
            if (avg > s_peak_value) {
                s_peak_value = avg;   // still rising, wait
            } else if (avg < TRIGGER_EXIT_THRESHOLD) {
                fired = true;         // word completed — fire
                s_peak_state = PeakState::IDLE;
            }
            break;
    }
    return fired;
}

bool kws_model_push_frame(const FeatureFrame& frame)
{
    // Shift the stack (simple ring-shift; STACK_FRAMES is small enough
    // that this is cheap relative to the 20 ms budget).
    if (s_stack_fill < STACK_FRAMES) {
        memcpy(s_feature_stack[s_stack_fill++], frame.mel, NUM_MEL_BINS);
        return false;   // not enough context yet
    }
    memmove(s_feature_stack[0], s_feature_stack[1],
            (STACK_FRAMES - 1) * NUM_MEL_BINS);
    memcpy(s_feature_stack[STACK_FRAMES - 1], frame.mel, NUM_MEL_BINS);

    return run_inference_and_smooth();
}

void kws_model_reset()
{
    std::fill(std::begin(s_recent_scores), std::end(s_recent_scores), 0.0f);
    s_score_idx = 0;
    s_peak_state = PeakState::IDLE;
    s_peak_value = 0.0f;
    s_stack_fill = 0;
}
