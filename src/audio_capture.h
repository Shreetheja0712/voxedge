#pragma once
#include <cstdint>

// Configures the I2S peripheral (INMP441, 16 kHz / 16-bit / mono) and starts
// the DMA-backed RX channel. Must be called once at boot, before the KWS
// task begins reading frames.
void audio_capture_init();

// Blocks the calling task until one hop's worth of new samples (HOP_LEN_BYTES)
// has been DMA'd in, then appends them to the ring buffer and advances
// write_ptr. This is the "20 ms interrupt" from the pipeline doc, expressed
// as a periodic i2s_channel_read() call rather than a raw ISR — the CPU only
// wakes for this call, everything between calls is DMA/hardware.
// Returns the new write_ptr (bytes) for convenience.
uint32_t audio_capture_read_next_hop();
