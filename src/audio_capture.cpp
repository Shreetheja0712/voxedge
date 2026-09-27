#include "audio_capture.h"
#include "pipeline_common.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include <cstring>

static const char* TAG = "audio_capture";
static i2s_chan_handle_t s_rx_chan = nullptr;

// INMP441 pin map — adjust to your board's wiring.
#define I2S_BCLK_PIN  GPIO_NUM_4
#define I2S_WS_PIN    GPIO_NUM_5
#define I2S_SD_PIN    GPIO_NUM_6

void audio_capture_init()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Two DMA descriptor buffers of one hop each -> "ping-pong" buffering
    // referenced in the resource budget (~2 KB).
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = HOP_LEN_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // INMP441 L/R pin tied to GND

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_LOGI(TAG, "I2S RX started: %lu Hz, mono, 16-bit", (unsigned long)SAMPLE_RATE_HZ);
}

uint32_t audio_capture_read_next_hop()
{
    static uint8_t hop_buf[HOP_LEN_BYTES];
    size_t bytes_read = 0;

    // Blocks until the DMA has a full hop ready. This is the only place
    // Core 0 touches the network... it doesn't: this call never talks to
    // Wi-Fi, satisfying the "Core 0 never blocks on Core 1" isolation rule.
    esp_err_t err = i2s_channel_read(s_rx_chan, hop_buf, HOP_LEN_BYTES,
                                      &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read != HOP_LEN_BYTES) {
        ESP_LOGW(TAG, "short I2S read: %d bytes (err=%d)", (int)bytes_read, err);
        return g_ring_buf.write_ptr;
    }

    uint32_t wp = g_ring_buf.write_ptr;
    uint32_t first_chunk = std::min<uint32_t>(HOP_LEN_BYTES, RING_BUFFER_BYTES - wp);
    memcpy(&g_ring_buf.data[wp], hop_buf, first_chunk);
    if (first_chunk < HOP_LEN_BYTES) {
        // wrap-around write
        memcpy(&g_ring_buf.data[0], hop_buf + first_chunk, HOP_LEN_BYTES - first_chunk);
    }
    wp = (wp + HOP_LEN_BYTES) % RING_BUFFER_BYTES;
    g_ring_buf.write_ptr = wp;  // single writer, no lock needed
    return wp;
}
