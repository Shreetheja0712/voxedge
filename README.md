# VoxEdge — Low-Latency Voice Activator for Edge Devices

> **SIH 2026 · Problem Statement 26172**
> Real-time keyword-spotted voice control on ESP32-S3, fully offline wake-word detection with < 256 KB SRAM, < 10% CPU at idle, and open-source only.

---

## Problem & Solution

Voice assistants today require always-on cloud connectivity, proprietary SDKs, and hardware that far exceeds what embedded IoT devices can afford. VoxEdge solves this with a **fully offline keyword spotting engine** running on a $4 ESP32-S3 microcontroller, paired with **on-demand cloud ASR** only after the wake word is detected.

### Key Value Propositions

| | |
|---|---|
| 🧠 **Privacy-first** | 100% offline KWS — cloud is contacted *only* after trigger |
| ⚡ **Ultra-low power** | < 10% CPU at idle; DMA handles audio capture with zero CPU |
| 💾 **Fits in 171 KB SRAM** | ~85 KB headroom on ESP32-S3's 256 KB budget |
| 🔑 **Custom wake word** | No "Hey Google" / "Alexa" — train your own keyword from scratch |
| 🔒 **AES-128-GCM** | Hardware-accelerated encryption, per-device eFuse keys |
| 📡 **< 300 ms trigger-to-ASR** | UDP + pre-open socket + paced catch-up burst |
| 🛠️ **Fully open-source** | TFLite Micro, Opus, Vosk/Kaldi, ESP-IDF, FreeRTOS — no proprietary SDK |

---

## Architecture Overview

The system implements a **7-step pipeline** documented in [`SIH26172_Complete_Pipeline.md`](SIH26172_Complete_Pipeline.md), running across the ESP32-S3's dual Xtensa LX7 cores under FreeRTOS:

```
┌─────────────────────── EDGE DEVICE (ESP32-S3) ──────────────────────┐
│                                                                      │
│  Core 0 — KWS_Task (HIGH PRIORITY, always running)                  │
│  ├── Step 1: I2S DMA → 32 KB ring buffer (continuous, zero CPU)     │
│  ├── Step 2: 30ms frame + Hanning window (50 frames/sec)            │
│  ├── Step 3: FFT → Mel filterbank → Log → Noise sub → INT8         │
│  ├── Step 4: DS-CNN inference → 30-frame temporal smooth → trigger  │
│  └── Step 6.7: VAD (reuses frame energy — zero extra cost)          │
│                                                                      │
│  Core 1 — Net_Task (LOWER PRIORITY)                                 │
│  ├── Step 5: Wi-Fi + UDP + AES-GCM init, NAT keep-alive            │
│  ├── Step 6: Catch-up burst + live Opus/RTP stream                  │
│  └── Step 7: Decrypt response → GPIO action → ACK → re-arm KWS     │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
                            │ UDP / RTP
                            ▼
┌────────────────────── CLOUD SERVER ─────────────────────────────────┐
│  Jitter buffer → Opus decode → Vosk/Kaldi ASR → intent → JSON      │
│  → AES-GCM encrypt → UDP response                                   │
└─────────────────────────────────────────────────────────────────────┘
```

**Critical isolation rule:** Core 0 (KWS) **never** calls any network function, never blocks on a socket, and never waits for Core 1. A dead Wi-Fi connection causes zero missed audio frames.

---

## Source File Map

| File | Pipeline Step(s) | Description |
|------|-----------------|-------------|
| [`pipeline_common.h`](src/pipeline_common.h) | All | Shared constants, ring buffer, state machine, cross-task handles |
| [`audio_capture.h/cpp`](src/audio_capture.cpp) | Step 1 | I2S + DMA capture (INMP441, 16kHz/16-bit/mono) → 32 KB ring buffer |
| [`dsp_features.h/cpp`](src/dsp_features.cpp) | Steps 2 & 3 | Framing, Hanning window, 512-pt FFT, 40-band Mel filterbank, log, noise subtraction, INT8 quantisation |
| [`kws_model.h/cpp`](src/kws_model.cpp) | Step 4 | TFLite Micro DS-CNN inference, 50-frame feature stack, 30-frame temporal moving average, three-state peak detector (0.70/0.60 hysteresis) |
| [`vad.h/cpp`](src/vad.cpp) | Step 6.7 | Energy-threshold VAD with adaptive noise floor — reuses `frame_energy` from DSP, no separate model |
| [`net_task.h/cpp`](src/net_task.cpp) | Steps 5, 6, 7 | Wi-Fi/UDP socket, AES-128-GCM (two-part nonce: NVS session + RAM packet counter), Opus+RTP streaming (catch-up burst + live), response decrypt/parse/GPIO/ACK |
| [`app_main.cpp`](src/app_main.cpp) | Boot + orchestration | Dual-core task spawn, Step 6.2 pre-roll boundary scan (chronological energy history), Core 0↔Core 1 notification |
| [`CMakeLists.txt`](CMakeLists.txt) | Build | Root ESP-IDF project config |
| [`src/CMakeLists.txt`](src/CMakeLists.txt) | Build | Component source/dependency registration |

---

## Resource Budget

| Segment | Allocation | Notes |
|---------|-----------|-------|
| KWS tensor arena | 32 KB | CNN activation scratchpad (peak usage) |
| Pre-roll audio ring buffer | 32 KB | 1.0 sec history — dual use: KWS + pre-roll |
| DMA ping-pong buffers | 2 KB | I2S double-buffering |
| UDP + AES-GCM + NVS nonce | 3 KB | Replaces TCP + mbedTLS (~28 KB → ~3 KB) |
| RTP/Opus encoder | 4 KB | Packetisation + codec state |
| RTOS stacks + heap | 32 KB | Net_Task needs ~6–8 KB for crypto call depth |
| Firmware logic | 16 KB | GPIO drivers, state machine, VAD |
| Wi-Fi driver buffers | 50 KB | MAC-layer; disable BT if unused |
| **Total** | **~171 KB** | |
| **Headroom** | **~85 KB** | Validate with `heap_caps_get_free_size()` |

---

## Before This Builds on Real Hardware

This is an **architectural skeleton** — it compiles against the *shape* of the pipeline but has project-specific gaps you must fill:

1. **`model_weights.h`** — Train the DS-CNN (TF/Keras + QAT) on your custom keyword dataset, convert with TFLite Converter (`TFLITE_BUILTINS_INT8`), then:
   ```bash
   xxd -i model.tflite > model_weights.h
   ```
2. **`mel_filterbank.h`** — Generate the 40-band Mel filterbank matrix offline (e.g. `librosa.filters.mel`) matching your training-time feature config, dump as a C array.
3. **AES key provisioning** — `s_device_aes_key` in `net_task.cpp` is a placeholder. Wire up real per-device eFuse key reads.
4. **Wi-Fi credentials** — `net_wifi_and_socket_init()` has a TODO where `esp_wifi_connect()` + event handlers need to go.
5. **GPIO action mapping** — `handle_server_response()` logs actions instead of calling `gpio_set_level()`; wire to your board's outputs.
6. **ESP-IDF Components** — Add via `idf.py add-dependency`:
   - `espressif/esp-dsp` (FFT + DSP helpers)
   - `espressif/esp-tflite-micro` (TFLite Micro runtime)
   - Fixed-point `libopus` build vendored as a component

None of these are pipeline-design gaps — they're inherently project-specific (your trained model, your keys, your board's GPIO map).

---

## Build

```bash
# One-time setup
idf.py set-target esp32s3
idf.py add-dependency "espressif/esp-dsp"
idf.py add-dependency "espressif/esp-tflite-micro"

# Build, flash, monitor
idf.py build flash monitor
```

---

## Technology Stack

| Component | Tool | Where Used |
|-----------|------|------------|
| Model training | TensorFlow / Keras | Step 4 — offline |
| Quantisation-Aware Training | `tensorflow_model_optimization` | QAT wrapper |
| Model export | TFLite Converter + `xxd` | `.tflite` → `model_weights.h` |
| Edge inference | TFLite for Microcontrollers (TFLM) | Bare-metal C++ on device |
| DSP math | ESP-DSP (CMSIS-DSP based) | FFT, matrix multiply |
| Audio codec | Opus (fixed-point) | Frame compression |
| Transport | RTP over UDP | Packetisation + ordering |
| Encryption | AES-128-GCM (HW accelerated) | Confidentiality + authenticity |
| RTOS | FreeRTOS (via ESP-IDF) | Task scheduling |
| Audio driver | ESP-IDF I2S + DMA | Capture |
| Cloud ASR | Vosk / Kaldi | Server-side speech recognition |

---

## Pipeline Document

For the complete, detailed rationale behind every architectural decision — memory budgets, nonce management, security model, why UDP not TCP, why DS-CNN, how VAD costs zero — see:

📄 [`SIH26172_Complete_Pipeline.md`](SIH26172_Complete_Pipeline.md)

---

## License

Open-source. All components used are open-source and license-compliant per the SIH problem statement requirements.
