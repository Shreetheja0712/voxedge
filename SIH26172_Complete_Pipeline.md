# SIH26172 — Complete End-to-End Pipeline: Design, Architecture & Workflow
### Low Latency and Efficient Voice Activator for Edge Devices
**Target Hardware:** ESP32-S3 | **Constraints:** < 256 KB SRAM, < 10 % CPU at idle, open-source only, custom keyword

---

## Master Pipeline Overview

Every architectural decision in this system flows from three hard constraints imposed by the problem statement:

1. **Memory** — entire runtime must fit in < 256 KB SRAM
2. **CPU** — continuous listening must consume < 10 % CPU
3. **Latency** — time from keyword-end to cloud ASR receiving audio must be minimal

The full end-to-end chain — from sound wave to hardware action — is:

```
 ┌─────────────────────────── EDGE DEVICE (ESP32-S3) ────────────────────────────────────┐
 │                                                                                        │
 │  Acoustic wave                                                                         │
 │      → MEMS mic (INMP441) → I2S bitstream                                             │
 │      → DMA → 32 KB circular buffer          [STEP 1: Voice Detection / Input]         │
 │      → CPU interrupt every 20 ms                                                      │
 │      → Hanning window → FFT → Mel filterbank → Log + noise reduction                  │
 │      → 40-D INT8 feature vector              [STEP 2 & 3: Voice Processing]           │
 │      → DS-CNN inference → 3-class softmax                                             │
 │      → 30-frame moving average → Peak detection                                       │
 │      → TRIGGER FIRED                         [STEP 4: Keyword Spotting Model]         │
 │                                                                                        │
 │  Core 0: compute read_ptr → signal Core 1 via xTaskNotifyGive()                       │
 │  Core 1: wakes from sleep                                                             │
 │      → UDP socket already open (idle state)  [STEP 5: Low-Latency Connection]        │
 │      → AES-128-GCM encrypt each frame                                                 │
 │      → Opus compress (640 B → ~50 B) → RTP wrap → UDP send                           │
 │      → Catch-up burst (backlog) + live stream [STEP 6: Audio Streaming]              │
 │      → VAD 800 ms silence → RTP marker bit → stream end                               │
 │                                                                                        │
 └────────────────────────────────────────────────────────────────────────────────────────┘
                                         │ UDP / RTP
                                         ▼
 ┌──────────────────────────── CLOUD SERVER ─────────────────────────────────────────────┐
 │  RTP jitter buffer → reorder by sequence number → Opus FEC reconstruct               │
 │  → Streaming ASR (Vosk / Kaldi) → transcript                                          │
 │  → Intent mapping → compact JSON command                                              │
 │  → AES-128-GCM encrypt → UDP send to device   [STEP 7: Response Handling]            │
 └────────────────────────────────────────────────────────────────────────────────────────┘
                                         │
                                         ▼
 ┌──────────────────── EDGE DEVICE — Response Handling ──────────────────────────────────┐
 │  Receive UDP packet → AES-128-GCM decrypt → parse JSON                               │
 │  → Execute hardware action (GPIO/relay) → LED/beep feedback                          │
 │  → Send 1-byte ACK → reset SYSTEM_STATE = IDLE_KWS → re-arm KWS                     │
 └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Dual-Core Task Architecture (FreeRTOS)

The ESP32-S3 has two Xtensa LX7 cores. The architecture isolates concerns cleanly:

```
Core 0 — KWS_Task (HIGH PRIORITY, always running)
├── I2S DMA audio capture (hardware, zero CPU)
├── 20 ms DSP interrupt: Hanning → FFT → Mel → Log → quantise
├── CNN inference → temporal smoother → peak detector
├── On trigger: compute read_ptr, signal Core 1
└── Immediately returns to listening — never blocks on network

Core 1 — Net_Task (LOWER PRIORITY)
├── Wi-Fi init (decoupled boot — starts after KWS is already running)
├── DHCP, DNS resolution, UDP socket creation
├── 25-second NAT keep-alive heartbeat
├── On wake signal: catch-up burst + live streaming
├── VAD end-detection → stream termination
└── Wi-Fi reconnect if disconnected (transparent, Core 0 unaffected)
```

**Critical isolation rule:** Core 0 (KWS) never calls any network function, never blocks on a socket, and never waits for Core 1. A dead Wi-Fi connection causes zero missed audio frames.

---

## STEP 1 — Voice Detection (Continuous Listening)

### Role
Hardware captures sound 100 % of the time. The CPU must not be involved per-sample — this is the foundation of the < 10 % CPU idle constraint.

### Hardware: INMP441 MEMS Microphone

The INMP441 is an omnidirectional MEMS digital microphone with an integrated sigma-delta ADC. It outputs a digital I2S bitstream directly — no external ADC required, no analog signal path, no analog noise.

| Parameter | Value | Reason |
|-----------|-------|--------|
| Sample rate | 16,000 Hz (16 kHz) | Captures full human speech band (0–8 kHz); no wasted bandwidth |
| Bit depth | 16 bits/sample | ±32,768 amplitude levels; sufficient SNR for speech |
| Channels | Mono | Wake-word needs no stereo; halves all downstream data |
| I2S BCLK | 256 kHz | = 16,000 × 16 bits × 1 channel |

### I2S Protocol — Three Wires, Zero CPU

```
BCLK  ─────┐  Bit clock — 256 kHz; synchronises every bit transfer
WS    ─────┤  Word Select — L/R channel flag (always L for mono)
SD    ─────┘  Serial Data — actual 16-bit sample, MSB first
```

Once configured, the I2S peripheral clocks in samples from the mic autonomously. Zero CPU instructions per sample.

### DMA Circular Buffer — The 1-Second "Time Machine"

The DMA controller autonomously reads I2S samples and writes them into a statically allocated 32 KB SRAM ring buffer. The CPU never touches individual samples.

```
Data rate = 16,000 samples/sec × 2 bytes/sample = 32,000 bytes/sec
Buffer    = 32 KB = 32,000 bytes  →  exactly 1.0 second of rolling history
```

```
 ┌──────────────────────────── 32 KB SRAM ────────────────────────────────┐
 │  [oldest — about to be overwritten] [newest audio] [older audio …]    │
 │                                          ↑                             │
 │                                     write_ptr (DMA)                    │
 └────────────────────────────────────────────────────────────────────────┘

write_ptr = (write_ptr + 2) % 32000    (increments by 2 bytes per 16-bit sample)
```

- Newest audio is always directly *behind* the pointer
- Oldest audio is directly *in front* (next to be overwritten)
- Net effect: at any moment, the buffer contains the most recent 1.0 second of audio

This buffer serves **triple duty** across the pipeline:
1. KWS DSP source (Steps 2–4)
2. Pre-roll cache (Step 5 — triggered audio history)
3. Live stream source (Step 6 — real-time forward)

---

## STEP 2 — Voice Input (Framing & Windowing)

### Role
Structure the raw PCM stream into analysis-ready frames. Raw 16,000 numbers/second carry no structure a CNN can learn from cheaply — this step organises them.

### DMA Interrupt — The Pipeline's Clock

DMA fires a hardware interrupt every **20 ms** (= 320 new samples = 640 bytes). On each interrupt, Core 0 pauses background tasks and executes one DSP cycle.

### Overlapping Frame Extraction

Rather than processing only the 20 ms of new audio, the CPU looks backward 30 ms from `write_ptr`:

```
Time →
│──────────────────── 30 ms window (480 samples) ──────────────────│
│  10 ms overlap with prev frame  │     20 ms new audio             │
                                  ↑                                  ↑
                          previous frame end               current write_ptr
```

- **30 ms window** — long enough to capture one full phoneme (~20–30 ms)
- **20 ms stride** — 50 frames/second, one per DMA interrupt
- **10 ms overlap** — audio events at frame boundaries appear in both adjacent frames; nothing is lost

### Hanning Window — Eliminating Spectral Leakage

Cutting 480 samples from a continuous stream creates abrupt mathematical edges. Applying FFT to an abrupt slice causes **spectral leakage**: energy from real frequencies bleeds into neighbouring bins, creating phantom frequencies that pollute CNN input.

```
w(n) = 0.5 × (1 − cos(2πn / (N−1)))     N = 480
```

This tapers both frame edges smoothly to zero:

```
Amplitude
  1.0 │          ╭─────────╮
      │       ╭──╯         ╰──╮
  0.5 │     ╭─╯               ╰─╮
      │  ╭──╯                   ╰──╮
  0.0 │──╯                         ╰──
      └─────────────────────────────────▶ Sample index (0 → 479)
```

---

## STEP 3 — Voice Processing & Noise Reduction (DSP Pipeline)

### Role
Convert the 480-sample time-domain frame into a 40-number vector encoding which speech-relevant frequencies are energetically present. This is the CNN's actual input — never the raw waveform.

All DSP runs via **ESP-DSP** — Espressif's own DSP library for the Xtensa LX7 architecture (`dsps_wind_hann_f32` for windowing, `dsps_fft2r_fc32` + `dsps_bit_rev2r_fc32` for the FFT, per-band dot products against the filterbank matrix for the Mel step). Earlier drafts of this doc referenced CMSIS-DSP's `arm_*` function names — those target ARM Cortex-M cores and don't run on Xtensa, so the firmware uses ESP-DSP's `dsps_*` API throughout; see `dsp_features.cpp`.

### 3.1 Fast Fourier Transform — Time → Frequency Domain

```
Input:  [s₀, s₁ … s₄₇₉]  — 480 windowed PCM samples (time domain)
Output: [E@0Hz, E@33Hz … E@8kHz]  — 240 frequency bins (frequency domain)
```

The FFT decomposes *how much energy is present at each frequency*:
- Vowels → strong energy at 300–2500 Hz (F1/F2 formants)
- Fricatives (s, sh, f) → broadband energy 3–8 kHz
- Plosives (p, t, k) → brief energy spikes across many bins
- Background noise → stable, slowly-varying frequency signature

The CNN learns to recognise keyword-shaped frequency patterns across all of these.

### 3.2 Mel Filterbank — Perceptual Compression (240 → 40)

Human hearing is logarithmic: we distinguish pitch differences easily below 2 kHz and poorly above 4 kHz. Linear FFT bins waste most resolution on frequencies that contribute little to speech discrimination.

**Mel scale:**
```
M(f) = 2595 × log₁₀(1 + f/700)
```

**How it works:** 40 overlapping triangular filters, equally spaced on the mel scale, each sum the FFT energy across their band:

```
Linear FFT bins (240) — equal spacing in Hz
│  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │ …
│←── wide filters at high freq ──→│←── narrow at low freq ──→│

Mel filterbank output: [m₀, m₁, m₂ … m₃₉]   (40 values)
```

Implementation: `arm_mat_mult_f32` multiplies the `[1×240]` FFT output by the `[240×40]` pre-calculated filterbank matrix (stored read-only in Flash — zero SRAM cost).

### 3.3 Logarithmic Scaling + Noise Reduction

**Log scaling:**
```
mel_log[i] = log₁₀(mel_energy[i] + ε)     (ε prevents log(0))
```

Two reasons:
1. **Dynamic range compression** — speech spans 6+ orders of magnitude (whisper → shout); log compresses this into a range INT8 can represent without clipping
2. **Perceptual match** — human loudness perception is also logarithmic; log-mel features align with how we actually hear

**Background noise subtraction:** A running estimate of steady-state background noise (fan hum, room tone, electrical hiss) is maintained per mel band and subtracted from each frame. Stationary noise cancels cleanly; the keyword's rapidly evolving pattern survives.

### 3.4 Output: The Feature Vector

One DSP cycle outputs a `[40]` float vector, generated every 20 ms.

Stack 50 consecutive vectors (50 × 20 ms = 1 second) → `[50 × 40]` 2D array — a **log-mel spectrogram**, visually like a small greyscale image:

```
Frequency (40 mel bands)
 40 │  ████░░░░░░░░░░████████░░░░░░░░░░░░░░
    │  ███░░░░░░░░████████████░░░░░░░░░░░░░
    │  ░░░░░░░░████░░░░░░░░░░█████████░░░░░
  1 │  ░░░░░░░████░░░░░░░░░░░░░░░░████░░░░░
    └─────────────────────────────────────── Time (50 frames = 1 sec)
```

The CNN finds the wake word's characteristic "shape" in this image.

### 3.5 INT8 Quantisation

Training uses FP32; the ESP32-S3 runs INT8 4× faster with 4× less RAM. **Quantisation-Aware Training (QAT):**

1. `tensorflow_model_optimization` inserts fake quantisation nodes during training — forward pass simulates INT8 rounding
2. Adam optimiser compensates during backprop, learning patterns robust to 8-bit precision loss
3. TFLite Converter reads the min/max ranges already captured by the fake quantisation nodes during training and uses those to calculate scale and zero_point — no representative dataset is needed
4. Result: every weight, activation, and input mapped to `[−128, 127]`

The DSP float vector is quantised to INT8 immediately before being written into the model's input tensor.

Step 1:  tensorflow_model_optimization wraps the Keras model,
         inserting fake quantisation nodes at every weight and activation.

Step 2:  During training, each forward pass snaps values to the INT8 grid.
         EMA observers inside each node track the running min/max of that
         tensor across all training batches.
         Adam compensates via backprop — weights converge to values that
         survive 8-bit rounding.

Step 3:  After training, the TFLite Converter reads the min/max ranges
         already embedded in the fake quantisation nodes.
         It uses these to calculate the exact scale and zero_point for
         every weight and activation tensor.
         No representative dataset is required.

Step 4:  Every weight, activation, and input is mapped to [−128, 127].
         The resulting .tflite file is a fully calibrated INT8 model.

```bash
xxd -i model.tflite > model_weights.h   # embed binary model as C byte array
```

---

## STEP 4 — Keyword Spotting Model (DS-CNN Inference + Temporal Logic)

### Role
Take the `[50 × 40 × 1]` INT8 tensor, output `P(wake_word)` every 20 ms, and convert raw probabilities into a single reliable binary trigger.

### 4.1 Why Depthwise-Separable CNN

Standard convolution cost: `9 × C_in × C_out` multiplications per output pixel.

DS-Conv splits into two steps:
```
Depthwise:  one (3×3) filter per channel, no cross-channel mixing
            Cost: 9 × C_in

Pointwise:  (1×1) convolution mixes channels
            Cost: C_in × C_out

Total DS: 9×C_in + C_in×C_out   vs.   9×C_in×C_out  (standard)
Savings:  ≈ 8–9× fewer multiplications
```

This single choice is what makes the model fit in < 256 KB Flash.

### 4.2 DS-CNN Architecture

**Input:** `[50, 40, 1]` INT8 (50 time frames × 40 mel bins × 1 channel)

| # | Layer | Config | Output Shape | Rationale |
|---|-------|--------|-------------|-----------|
| 0 | Input | `[50, 40, 1]` | `[50, 40, 1]` | 1 sec log-mel spectrogram |
| 1 | Conv2D | `filters=16, kernel=(10,4), stride=(2,2)` | `[21, 19, 16]` | Wide time kernel detects vowel formants; stride halves tensor |
| 2 | DS-Conv | `kernel=(3,3), pointwise=16` | `[21, 19, 16]` | Low-level phoneme edge detection |
| 3 | DS-Conv | `kernel=(3,3), pointwise=32` | `[21, 19, 32]` | Phoneme transitions (vowel → consonant) |
| 4 | DS-Conv | `kernel=(3,3), pointwise=32` | `[21, 19, 32]` | Full 1-sec temporal consolidation |
| 5 | GlobalAvgPool2D | all | `[32]` | Collapses spatial dims; avoids Dense weight explosion |
| 6 | Dense + Softmax | `units=3` | `[3]` | `P(silence)`, `P(unknown)`, `P(wake_word)` |

**Why GlobalAvgPool2D not Flatten:** Flatten after last DS-Conv gives `21×19×32 = 12,768` values → Dense matrix `12,768×3 = 38,304` weights = ~153 KB of SRAM. GlobalAvgPool collapses to 32 values → Dense matrix `32×3 = 96` weights. Negligible.

### 4.3 TFLM Inference — Bare-Metal C++

```
Static firmware memory layout:
┌────────────────────────────────────────────────┐
│  model_weights.h      (Flash, read-only)       │  ~4–80 KB
│  tensor_arena[40KB]   (SRAM)                   │  CNN activation scratchpad
│  audio_buffer[32KB]   (SRAM)                   │  1-sec circular buffer
│  mel_filterbank       (Flash, read-only)       │  pre-computed matrix
└────────────────────────────────────────────────┘
```

Per-frame inference loop (every 20 ms, on DMA interrupt):

```
1.  DMA interrupt fires (Core 0)
2.  Read 480 samples from circular buffer (30 ms lookback)
3.  Apply Hanning window  (ESP-DSP: arm_mult_f32)
4.  FFT → 240 bins        (arm_rfft_fast_f32)
5.  × mel filterbank → 40 values  (arm_mat_mult_f32)
6.  log() + noise subtraction → 40 log-mel floats
7.  Quantise FP32 → INT8
8.  Write to input_tensor->data.int8[current_frame * 40]
9.  Call interpreter->Invoke()
10. Read output_tensor->data.int8[2] → P(wake_word)
11. Feed to temporal smoother
```

### 4.4 Temporal Smoothing (30-frame moving average = 600 ms)

A single CNN frame can spike due to noise, unrelated syllables, or interference. Acting on it = false positive.

```
smoothed_score[t] = (1/30) × Σ P(wake_word)[t-i]   for i = 0..29
```

600 ms matches the duration of a spoken wake word. The smoothed score only rises above threshold if the CNN sees sustained evidence across the entire word.

**Trigger condition:** `smoothed_score > 0.70`

### 4.5 Peak Detection Algorithm — Precise Trigger Timing

Triggering immediately when the threshold is crossed cuts the final syllable and clips the start of the command. The peak detector waits for the word to fully complete:

```
State: IDLE

Every 20 ms frame:
  IDLE      + smoothed > 0.70  →  state = TRACKING, peak = smoothed
  TRACKING  + smoothed > peak  →  peak = smoothed   (still rising, wait)
  TRACKING  + smoothed < 0.60  →  FIRE TRIGGER, state = IDLE
```

The decisive drop below 60 % signals the acoustic pattern has concluded. The trigger fires after the word ends, before silence, before the next word.

### 4.6 Trigger Handoff (Core 0 → Core 1)

```cpp
// Core 0: on trigger fire
volatile uint32_t shared_read_ptr = compute_read_ptr();   // 1.0s lookback
xTaskNotifyGive(core1_streaming_handle);                  // wake-up signal only
// Core 0 immediately resumes monitoring — does NOT block
```

`xTaskNotifyGive()` is used purely as a wake-up signal (increments a counter). If a value needs to travel with the signal, `xTaskNotify(..., eSetValueWithOverwrite)` is used. The `volatile` qualifier ensures Core 1 reads the latest `read_ptr` value after waking.

---

## STEP 5 — Low-Latency Connection to Cloud ASR Server

### Role
Establish and maintain a permanently ready, low-overhead network channel to the cloud ASR server so that when the trigger fires, transmission begins **immediately** — no connection setup latency at trigger time.

### 5.1 Decoupled Boot Sequence

The KWS engine must never wait on the network stack. Boot sequence:

```
Power-on
  │
  ├─► Core 0: spawn KWS_Task (HIGH priority)
  │         → I2S DMA begins immediately
  │         → KWS is listening before Wi-Fi starts
  │
  └─► Core 1: spawn Net_Task (LOWER priority)
            → Wi-Fi station mode (WIFI_MODE_STA)
            → Non-blocking DHCP (wait on SYSTEM_EVENT_STA_GOT_IP event — no polling)
            → DNS resolution: one-time lookup of cloud endpoint
            → UDP socket creation (~1 KB SRAM)
            → AES-128-GCM init
            → Ready state: NAT keep-alive loop begins
```

If the trigger fires before Net_Task is ready: device signals failure immediately via distinct LED/beep pattern rather than failing silently.

### 5.2 Why UDP, Not TCP

| | TCP | UDP (chosen) |
|--|-----|------|
| Send buffer | ~8–16 KB | None |
| Receive window state | ~4 KB | None |
| Congestion control | Yes (state machine) | No |
| Retransmission queue | Yes | No |
| SRAM cost | ~20–28 KB | ~1 KB |
| Handshake on connect | Yes (RTT penalty) | No |
| Suitable for real-time audio | No — retransmit stalls | Yes — loss tolerated via FEC |

For real-time audio streaming, a lost packet is better skipped (recovered by Opus FEC) than waited on (TCP retransmit stalls the decoder by hundreds of ms).

### 5.3 Security: AES-128-GCM (No TLS)

TLS requires certificate parsing, asymmetric crypto handshake, and ~28 KB of RAM for mbedTLS state. The system replaces this entirely with hardware-accelerated symmetric crypto:

```
Device provisioning (one-time at manufacture):
  → Each device flashed with a UNIQUE 128-bit AES key in eFuse
  → Compromising one device does NOT compromise the fleet

Runtime (AES-128-GCM):
  → Hardware crypto accelerator on ESP32-S3 — near-zero CPU cost
  → No certificate parsing, no handshake, no asymmetric operations
  → SRAM: ~3 KB total (socket + AES-GCM context + NVS counter)
```

**Nonce management (critical):**

GCM is catastrophically broken if the same (key, nonce) pair is ever reused — full key recovery and forgery become possible. The standard AES-GCM nonce is 96 bits.

⚠️ Flaw in the original design: an earlier version of this pipeline stored a single counter in NVS (flash) and rewrote it on every encrypted packet. At ~50 packets/sec during streaming that is ~180,000 flash writes per hour — far beyond safe flash write-endurance — and each write's erase-before-write latency (millisecond-scale) is incompatible with the 20 ms audio frame budget. Corrected below.

Corrected two-part nonce construction:

96-bit Nonce = 32-bit session/boot counter   (persisted to NVS — written ONCE per boot)
             + 64-bit packet counter          (RAM only — increments per packet, resets each session)
- Session counter: read once at boot, incremented, written back to NVS a single time.
  Guarantees no nonce repeats across reboots.
- Packet counter: lives entirely in RAM, increments freely at streaming speed (~50/sec).
  Zero flash writes during streaming — no wear, no per-packet latency cost.
- Together: the (key, nonce) pair is never reused, at effectively zero runtime cost.
- Separate from the RTP sequence number (which resets per session — transport concern only).

Replay protection (added): the GCM authentication tag proves a packet is genuine and untampered — it does not prove the packet is fresh. A captured packet could be resent later and would still pass authentication. The receiver (device and server) maintains a sliding replay window: track the highest packet counter seen, accept anything within a window behind it (e.g. last 64 sequence numbers, matching Opus FEC's reordering tolerance), and reject anything older or already seen. A strict "must strictly increase" check is avoided since legitimate UDP/RTP reordering happens in normal operation.

For pitch/slide use: AES-128-GCM + Unique Nonce + Replay Protection.

### 5.4 NAT Keep-Alive (25-second Heartbeat)

Many carrier-grade NATs expire UDP mappings after ~30 seconds of inactivity. Net_Task sends a 4-byte UDP heartbeat every **25 seconds** to keep the router mapping alive. This ensures when the trigger fires, the UDP path to the server is already open — no NAT re-establishment delay.

```
Net_Task idle loop:
  while(1) {
      vTaskDelay(25000 / portTICK_PERIOD_MS);
      udp_send(heartbeat, 4 bytes);
  }
```

### 5.5 Wi-Fi Reconnect (Transparent to KWS)

If Wi-Fi drops, Net_Task handles DHCP reconnection and DNS re-resolution silently in the background. KWS_Task never knows or cares — it never calls any network function.

---

## STEP 6 — Streaming Audio Data to Cloud ASR Server

### Role
After the trigger fires, deliver the complete spoken command — including the audio that occurred during the trigger detection delay (pre-roll) and all subsequent live audio — to the cloud ASR, with minimal latency and zero audio loss.

### 6.1 The Pre-Roll Problem

The peak detection algorithm waits for the wake word to fully complete before firing the trigger. This introduces an inherent delay of 200–400 ms. By the time the trigger fires, the user has likely already begun saying the next word ("...turn on the lights").

If streaming starts from the current timestamp, the first syllable of the command is lost. The solution: the 32 KB circular buffer is already a pre-roll cache containing the last 1.0 second of audio.

### 6.2 Computing the Pre-Roll Boundary (Dynamic read_ptr)

A fixed time offset (e.g., "go back 500 ms") cannot handle users of varying speaking speeds. Instead, the system locates the exact silence boundary between the wake word and the command:

```
During DSP phase (every frame, Step 3):
  frame_energy[t] = sum(mel_log_vector[t])   // 40-bin energy sum → scalar

On trigger fire:
  1. Scan frame_energy[] backwards from CNN peak
  2. Find absolute minimum (the silence/breath between wake word and command)
  3. Find where energy rises back up after that minimum
  4. That rising edge = start of the command
  5. Compute byte offset in 32 KB buffer → read_ptr
```

This accommodates slow and fast speakers equally. The `read_ptr` looks back up to 1.0 second — comfortably beyond the CNN's ~620 ms receptive field plus its 600 ms smoothing window.

### 6.3 Audio Compression: Opus (Fixed-Point)

Raw PCM is 640 bytes per 20 ms frame. Streaming at this rate = 32,000 bytes/sec. Opus compresses dramatically:

```
Raw PCM:      640 bytes / 20 ms frame  (32,000 bytes/sec)
Opus output:  ~40–60 bytes / 20 ms frame  (~2,400 bytes/sec)
Compression:  ~10–13× reduction

Opus configuration:
  - Fixed-point build  (ESP32-S3 has no hardware double-precision FPU)
  - VoIP mode          (optimised for voice, not music)
  - Variable bitrate   (adjusts to content complexity)
  - In-band FEC        (repair single dropped packets from next packet's payload)
```

### 6.4 RTP Packetisation

Each compressed Opus frame is wrapped in a 12-byte RTP (Real-time Transport Protocol) header:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┼─┤
│V=2│P│X│  CC   │M│    PT     │        Sequence Number            │
├───┴─┴─┴───────┴─┴───────────┴───────────────────────────────────┤
│                        Timestamp                                  │
├──────────────────────────────────────────────────────────────────┤
│                    Synchronization Source (SSRC)                  │
└──────────────────────────────────────────────────────────────────┘
  M bit = 1 on the final packet only (end-of-utterance marker)
  Sequence number: per-session counter (resets per stream, separate from nonce)
```

After RTP wrapping, the packet is encrypted with AES-128-GCM (nonce counter incremented) and sent over UDP.

### 6.5 Catch-Up Burst (Phase 1 of Streaming)

Core 1 wakes up and immediately begins transmitting the historical backlog from `read_ptr`:

```
Historical backlog: up to 50 frames × 640 bytes = 32,000 bytes raw
After Opus:         up to 50 frames × ~50 bytes  = ~2,500 bytes
After RTP + AES:    ~2,500 + overhead             = ~3,000 bytes
```

**Pacing:** The backlog is NOT sent as a single instant burst. It is sent at **4–5× real-time rate**:
- Full 1.0s backlog clears in ~200–250 ms
- Avoids overflowing the local Wi-Fi TX queue
- Avoids handing the server's jitter buffer a burst pattern it cannot smooth
- Wi-Fi throughput (~5–10 Mbps) vastly exceeds audio data rate (~2,400 bytes/sec) — 4–5× real-time is trivially achievable

### 6.6 Live Stream Handoff (Phase 2 of Streaming)

Once `read_ptr` catches up to the live DMA `write_ptr`:

```
Every 20 ms (I2S DMA interrupt):
  Core 0: DMA writes 640 bytes at write_ptr → advances write_ptr
  Core 1: encodes frame → RTP wrap → AES-GCM encrypt → UDP send
          read_ptr = write_ptr   (in sync)
```

Zero latency, zero audio loss. The transition from catch-up to live stream is seamless — the server sees a continuous RTP sequence with no gap.

### 6.7 Stream Termination: Voice Activity Detection (VAD) — Zero-Cost, Not a Separate Model

⚠️ Design decision: a dedicated VAD stage could be built as its own small neural classifier (e.g. a WebRTC-VAD-style GMM or a compact NN, as several open KWS references use). That path was rejected for this system — a second model means a second Flash footprint, a second inference pass every frame, and its own SRAM activation buffer, all working directly against the < 256 KB budget the entire architecture is built around.

Corrected: VAD is not a model at all. `frame_energy[t]` is already computed as an intermediate value of Step 3's Log-Mel extraction (Section 3, per-band energy sum) for every frame, regardless of whether VAD exists — the CNN needs it too. VAD costs one extra comparison per frame against `noise_floor_estimate`, plus a persistent `silence_frame_count` integer. No extra tensor, no extra weights, no measurable extra SRAM or CPU. This is why the Resource Budget table has no separate VAD line — it's folded into the 16 KB "Firmware application logic" bucket alongside the state machine and GPIO drivers.

Core 0 evaluates frame energy in parallel with running the CNN:

```
VAD logic (Core 0, every frame):
  if frame_energy[t] < noise_floor_estimate:
      silence_frame_count++
  else:
      silence_frame_count = 0

  if silence_frame_count >= 40:   // 40 frames × 20 ms = 800 ms
      signal Core 1: "end of utterance"
```

800 ms threshold — unified with the server's own endpoint logic to avoid two uncoordinated detectors (previously a bug: 500 ms local vs 800 ms server-side, making the server's detector unreachable since the local side would already close the stream).

**Stream closure (Core 1):**
```
1. Send final RTP packet with Marker bit = 1
2. Flush Opus encoder (send any buffered frames)
3. Close UDP socket
4. Signal Core 0: streaming complete
```

The server treats the Marker bit as the primary end-of-utterance trigger. Its own independent silence detection acts only as a fallback in case the final packet is lost.

---

## STEP 7 — Response Handling from Cloud ASR Server

### Role
Receive the server's intent response, decrypt and parse it, execute the hardware action, acknowledge receipt, and return the system to idle listening state.

### 7.1 Cloud-Side Processing (Server)

```
Incoming UDP/RTP packets
  → Jitter buffer: reorder by RTP sequence number
  → Opus decoder + FEC: reconstruct isolated dropped frames
  → Streaming ASR (Vosk or Kaldi — open-source, license-compliant)
  → Transcript produced on receipt of Marker bit (or fallback silence timeout)
  → Intent mapping: transcript → compact JSON command
  → AES-128-GCM encrypt (same per-device key, own nonce counter)
  → UDP send to device
```

**ASR engine selection:** Vosk and Kaldi are chosen because they are **genuinely open-source**. NVIDIA Riva is excluded — it is proprietary and violates the problem statement's open-source requirement.

### 7.2 Device-Side Response Reception

```
1. Receive UDP packet on open socket
2. AES-128-GCM decrypt (verify authentication tag — reject forged packets)
3. Parse JSON payload:
   {
     "action": "GPIO_SET",
     "pin": 5,
     "state": 1,
     "feedback": "beep_confirm"
   }
4. Execute hardware action (GPIO/relay)
5. Provide user feedback: LED indicator or audio beep
6. Send 1-byte ACK to server immediately
```

### 7.3 Reliability: Command ACK + Server Retry

Audio frames tolerate loss via Opus FEC — a single dropped audio packet is reconstructed from the next. The command packet (the actual instruction) does not tolerate loss — a dropped GPIO command means the device does nothing without warning.

```
Device:  receives command → sends 1-byte ACK within ~50 ms
Server:  waits for ACK → timeout ~300 ms → retry
         Max 3 retries → give up → device signals failure (LED/beep)
```

This asymmetry is intentional: audio frames use FEC (forward error correction, no retransmit), while the command uses ARQ (automatic repeat request, explicit ACK/retry).

### 7.4 State Reset and KWS Re-Arm

```cpp
// After ACK sent:
SYSTEM_STATE = IDLE_KWS;
kws_temporal_smoother.reset();    // clear 30-frame moving average
kws_peak_detector.reset();        // clear TRACKING state
Core0_KWS_Task resumes normal listening loop
```

The system is fully re-armed. The entire pipeline — from this wake event to the next — has left no residual state.

---

## Complete End-to-End Data Flow Summary

| Step | Stage | Input | Key Operation | Output | Timing |
|------|-------|-------|---------------|--------|--------|
| 1 | Voice Detection | Acoustic pressure | MEMS + I2S + DMA | 32 KB rolling PCM buffer | Continuous |
| 2 | Voice Input | PCM stream | Frame (30 ms) + Hanning window | 480 windowed samples | 50/sec |
| 3 | Voice Processing | 480 samples | FFT → Mel → Log → Noise sub → Quantise | 40-D INT8 vector | 50/sec |
| 3→4 | Feature Accumulation | INT8 vectors | Stack 50 frames | `[50×40×1]` tensor | 50/sec |
| 4 | KWS Inference | `[50×40×1]` INT8 | DS-CNN forward pass | 3 softmax probabilities | 50/sec |
| 4 | Temporal Logic | `P(wake_word)` | 30-frame moving avg + peak detect | TRIGGER event | Event |
| 5 | Network (idle) | Boot event | UDP socket + AES-GCM init | Ready channel | Once |
| 5 | Pre-Roll | TRIGGER + ring buffer | Energy scan → `read_ptr` | Boundary timestamp | <1 ms |
| 6 | Catch-Up Burst | Historical audio | Opus encode → RTP → AES-GCM → UDP | Paced historical stream | ~200 ms |
| 6 | Live Stream | New DMA frames | Opus encode → RTP → AES-GCM → UDP | Real-time stream | 20 ms/frame |
| 6 | VAD / Termination | Frame energy | 800 ms silence detect | RTP Marker bit + close | Event |
| 7 | Cloud ASR | UDP/RTP audio | Jitter buf → Opus decode → Vosk/Kaldi | JSON command | ~200–500 ms |
| 7 | Response Handling | JSON command | Decrypt → parse → GPIO + ACK | Hardware action | <50 ms |
| 7 | State Reset | ACK sent | Re-arm KWS | Back to IDLE_KWS | Immediate |

---

## Resource Budget (Full System, ESP32-S3)

| Segment | Allocation | Notes |
|---------|-----------|-------|
| KWS tensor arena | 32 KB | CNN activation scratchpad (peak usage) |
| Pre-roll audio ring buffer | 32 KB | 1.0 sec history — dual use: KWS source + pre-roll |
| DMA ping-pong buffers | 2 KB | I2S double-buffering |
| UDP socket + AES-GCM context + NVS nonce counter | 3 KB | Replaces TCP + mbedTLS (~28 KB → ~3 KB) |
| RTP/Opus encoder buffers | 4 KB | Packetisation + codec state |
| RTOS task stacks + system heap | 32 KB | Net_Task needs ~6–8 KB for crypto/network call depth |
| Firmware application logic | 16 KB | GPIO drivers, state machine, VAD |
| Wi-Fi/BT driver internal buffers | 50 KB | MAC-layer buffers, independent of TCP/UDP choice; **disable BT if unused** |
| **Total** | **~171 KB** | |
| **Headroom** | **~85 KB** | Honest margin; validate with `heap_caps_get_free_size()` on real hardware |

**Flash (not SRAM):** KWS model weights (~4 KB INT8), mel filterbank matrix (~4 KB), firmware (~200 KB) — all in Flash.

---

## Problem Statement Compliance Verification

| Requirement | Solution | Status |
|-------------|----------|--------|
| < 256 KB RAM | ~171 KB allocated; ~85 KB headroom | ✅ |
| < 10 % CPU at idle | DMA handles audio; Core 0 sleeps between 20 ms DSP bursts | ✅ |
| Open-source only | TFLite Micro, CMSIS-DSP, Opus, Vosk/Kaldi, ESP-IDF, FreeRTOS | ✅ |
| No proprietary KWS SDK | No PocketSphinx commercial, no Sensory TrulyHandsfree, no NVIDIA Riva | ✅ |
| No "Hey Google" / "Alexa" pre-trained model | Custom keyword, trained from scratch with QAT | ✅ |
| Custom keyword support | DS-CNN trained on custom dataset; 3-class output (silence/unknown/keyword) | ✅ |
| Runs on ESP32 or Raspberry Pi | TFLite Micro (bare-metal C++) + ESP-IDF confirmed for ESP32-S3 | ✅ |
| High true-positive rate | 30-frame (600 ms) temporal smoothing + 70 % threshold eliminates spurious triggers | ✅ |
| Near-zero false activations | Peak detection waits for word completion; sustained score required, not single-frame | ✅ |
| Low latency (keyword-end → ASR receives audio) | UDP (no handshake) + pre-open socket + paced catch-up burst ≈ < 300 ms total | ✅ |
| Audio loss prevention | Pre-roll cache captures audio during detection delay; dynamic boundary finds command start | ✅ |
| Privacy (no always-on cloud) | 100 % offline KWS; cloud contacted only post-trigger | ✅ |

---

## Framework & Toolchain Summary

| Component | Tool | Where used |
|-----------|------|------------|
| Model training | TensorFlow / Keras | Step 4 — offline, on development machine |
| Quantisation-Aware Training | `tensorflow_model_optimization` | Step 4 — QAT wrapper |
| Model export | TFLite Converter (`TFLITE_BUILTINS_INT8`) + `xxd` | Step 4 — `.tflite` → `model_weights.h` |
| Edge inference runtime | TFLite for Microcontrollers (TFLM) | Step 4 — bare-metal C++ on device |
| DSP math | ARM CMSIS-DSP | Step 3 — FFT, matrix multiply |
| Audio codec | Opus (fixed-point build) | Step 6 — frame compression |
| Transport protocol | RTP over UDP | Step 6 — packetisation + ordering |
| Encryption | AES-128-GCM (hardware accelerated) | Steps 6 & 7 — confidentiality + authenticity |
| RTOS | FreeRTOS (via ESP-IDF) | All steps — task scheduling |
| Audio peripheral driver | ESP-IDF I2S + DMA | Steps 1 & 2 |
| Cloud ASR engine | Vosk or Kaldi | Step 7 — server-side |

---

## Open Items — Validate on Real Hardware Before Locking

1. **Actual heap measurement:** Run `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` immediately after Wi-Fi + AES-GCM init on a real ESP32-S3 with BT disabled and Wi-Fi buffer counts tuned down in `menuconfig`. Replaces every SRAM estimate above with a measured fact.
2. **Model accuracy at ~4 KB parameter count:** Validate true-positive / false-accept rate on your actual custom keyword before committing to that Flash budget. A larger model (up to ~80 KB in Flash) may be needed.
3. **eFuse key provisioning pipeline:** Confirm per-device AES key provisioning is wired into the build/flash pipeline before demo day — not just planned.
4. **NAT keep-alive tuning:** 25 seconds is conservative; measure the actual NAT timeout of your demo venue's router and adjust.
5. **Opus FEC validation:** Confirm single-packet loss recovery works end-to-end with Vosk/Kaldi's RTP ingest — FEC helps only if the decoder uses it.