#include "vad.h"
#include "pipeline_common.h"

static float s_noise_floor_estimate = 0.0f;
static uint32_t s_silence_frame_count = 0;
static bool s_noise_floor_init = false;

bool vad_push_frame(float frame_energy)
{
    // Initialise noise floor from first frame's energy level
    if (!s_noise_floor_init) {
        s_noise_floor_estimate = frame_energy;
        s_noise_floor_init = true;
    }

    // Adaptive noise floor: slowly tracks ambient level.
    // Only adapts on frames close to or below the current floor,
    // so speech doesn't drag the estimate upward.
    if (frame_energy < s_noise_floor_estimate * 1.5f + 5.0f) {
        s_noise_floor_estimate = 0.95f * s_noise_floor_estimate + 0.05f * frame_energy;
    }

    if (frame_energy < s_noise_floor_estimate * 1.2f + 2.0f) {
        s_silence_frame_count++;
    } else {
        s_silence_frame_count = 0;
    }

    if (s_silence_frame_count >= VAD_SILENCE_FRAMES) {
        return true;   // 800 ms silence -> "end of utterance"
    }
    return false;
}

void vad_reset()
{
    s_silence_frame_count = 0;
    // Keep noise floor estimate across utterances — it represents
    // the room's ambient level, not the speech level.
}
