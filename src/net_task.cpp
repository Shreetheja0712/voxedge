#include "net_task.h"
#include "pipeline_common.h"
#include "vad.h"
#include "kws_model.h"

#include <cstring>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/gcm.h"
#include "cJSON.h"

// libopus (fixed-point build) — vendor as a component; see README.
extern "C" {
#include "opus.h"
}

static const char* TAG = "net_task";

// ---- Config: replace with your ASR server + per-device provisioning ----
#define ASR_SERVER_IP   "192.168.1.50"
#define ASR_SERVER_PORT 5005
static uint8_t s_device_aes_key[16];   // TODO: read from eFuse at boot, not hardcode

// ---- RTP header (Step 6.4) ----
#pragma pack(push, 1)
struct RtpHeader {
    uint8_t  v_p_x_cc = 0x80;        // V=2, P=0, X=0, CC=0
    uint8_t  m_pt;                    // M bit | payload type
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
};
#pragma pack(pop)

static int s_udp_sock = -1;
static struct sockaddr_in s_server_addr;
static mbedtls_gcm_context s_gcm_ctx;
static uint32_t s_session_counter = 0;   // 32-bit, NVS-persisted, written ONCE per boot
static uint64_t s_packet_counter  = 0;   // 64-bit, RAM only, resets per session
static uint16_t s_rtp_seq = 0;
static uint32_t s_rtp_timestamp = 0;     // audio-sample counter, reset per stream
static OpusEncoder* s_opus_enc = nullptr;

// ---------------------------------------------------------------------
// Step 5.1 / 5.3 — Wi-Fi bring-up, UDP socket, AES-GCM + nonce init
// ---------------------------------------------------------------------
static void load_session_counter_from_nvs()
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("voxedge", NVS_READWRITE, &h));
    uint32_t val = 0;
    if (nvs_get_u32(h, "sess_ctr", &val) != ESP_OK) val = 0;
    val++;                                   // new boot -> new session
    nvs_set_u32(h, "sess_ctr", val);         // single flash write per boot
    nvs_commit(h);
    nvs_close(h);
    s_session_counter = val;
}

static void build_nonce(uint8_t out_nonce[12])
{
    // 96-bit nonce = 32-bit session counter || 64-bit packet counter.
    // Session counter is fixed for the life of this boot; packet counter
    // increments per packet in RAM only (zero flash writes while streaming).
    memcpy(out_nonce, &s_session_counter, 4);
    memcpy(out_nonce + 4, &s_packet_counter, 8);
    s_packet_counter++;
}

static void net_wifi_and_socket_init()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    load_session_counter_from_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    // TODO: wire up SYSTEM_EVENT_STA_GOT_IP handler + wifi_config_t with
    // your SSID/password (or provisioning flow), then esp_wifi_start()/connect().
    // Non-blocking: KWS task on Core 0 must never wait on this.

    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&s_server_addr, 0, sizeof(s_server_addr));
    s_server_addr.sin_family = AF_INET;
    s_server_addr.sin_port   = htons(ASR_SERVER_PORT);
    inet_pton(AF_INET, ASR_SERVER_IP, &s_server_addr.sin_addr);

    mbedtls_gcm_init(&s_gcm_ctx);
    mbedtls_gcm_setkey(&s_gcm_ctx, MBEDTLS_CIPHER_ID_AES, s_device_aes_key, 128);

    s_opus_enc = opus_encoder_create(SAMPLE_RATE_HZ, 1, OPUS_APPLICATION_VOIP, nullptr);
    opus_encoder_ctl(s_opus_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_PACKET_LOSS_PERC(10));

    ESP_LOGI(TAG, "Net ready: UDP socket + AES-GCM + Opus initialised (session=%lu)",
             (unsigned long)s_session_counter);
}

static void send_heartbeat_if_due()
{
    static TickType_t last = 0;
    TickType_t now = xTaskGetTickCount();
    if ((now - last) * portTICK_PERIOD_MS >= 25000) {
        const char hb[4] = {0};
        sendto(s_udp_sock, hb, sizeof(hb), 0,
               (struct sockaddr*)&s_server_addr, sizeof(s_server_addr));
        last = now;
    }
}

// ---------------------------------------------------------------------
// Step 6.4 / 6.5 — encode + encrypt + send one frame
// ---------------------------------------------------------------------
static void send_audio_frame(const int16_t* pcm, uint32_t n_samples, bool is_final)
{
    uint8_t opus_buf[240];
    int opus_len = opus_encode(s_opus_enc, pcm, n_samples, opus_buf, sizeof(opus_buf));
    if (opus_len < 0) { ESP_LOGW(TAG, "opus encode failed: %d", opus_len); return; }

    RtpHeader hdr{};
    hdr.m_pt = is_final ? 0x80 : 0x00;    // M bit set on final packet only
    hdr.seq  = htons(s_rtp_seq++);
    hdr.timestamp = htonl(s_rtp_timestamp);  // dedicated audio-sample counter
    s_rtp_timestamp += n_samples;
    hdr.ssrc = htonl(0xC0FFEE);           // per-device SSRC

    uint8_t plaintext[sizeof(RtpHeader) + sizeof(opus_buf)];
    memcpy(plaintext, &hdr, sizeof(hdr));
    memcpy(plaintext + sizeof(hdr), opus_buf, opus_len);
    size_t plain_len = sizeof(hdr) + opus_len;

    uint8_t nonce[12];
    build_nonce(nonce);
    uint8_t ciphertext[sizeof(plaintext)];
    uint8_t tag[16];
    mbedtls_gcm_crypt_and_tag(&s_gcm_ctx, MBEDTLS_GCM_ENCRYPT, plain_len,
                               nonce, sizeof(nonce), nullptr, 0,
                               plaintext, ciphertext, sizeof(tag), tag);

    uint8_t wire_buf[12 + sizeof(plaintext) + sizeof(tag)];
    memcpy(wire_buf, nonce, 12);
    memcpy(wire_buf + 12, ciphertext, plain_len);
    memcpy(wire_buf + 12 + plain_len, tag, sizeof(tag));

    sendto(s_udp_sock, wire_buf, 12 + plain_len + sizeof(tag), 0,
           (struct sockaddr*)&s_server_addr, sizeof(s_server_addr));
}

// ---------------------------------------------------------------------
// Step 6.5 — catch-up burst: drain ring buffer from read_ptr to write_ptr
// at ~4-5x real time, then Step 6.6 — hand off to live streaming.
// Step 6.7 — VAD-driven close is polled every hop via g_trigger_msg's
// silence-notify path (the KWS task calls vad_push_frame() and notifies
// this task via NOTIFY_BIT_END_OF_UTT — see kws side of the pipeline).
// ---------------------------------------------------------------------
static void stream_utterance(uint32_t read_ptr)
{
    g_system_state = SystemState::STREAMING;
    s_rtp_seq = 0;
    s_rtp_timestamp = 0;   // reset per-stream counters

    // Phase 1: catch-up burst, paced at ~4-5× real time (not an instant dump).
    while (read_ptr != g_ring_buf.write_ptr) {
        int16_t pcm[HOP_LEN_SAMPLES];
        for (uint32_t i = 0; i < HOP_LEN_SAMPLES; ++i) {
            uint32_t off = (read_ptr + i * BYTES_PER_SAMPLE) % RING_BUFFER_BYTES;
            memcpy(&pcm[i], &g_ring_buf.data[off], sizeof(int16_t));
        }
        read_ptr = (read_ptr + HOP_LEN_BYTES) % RING_BUFFER_BYTES;
        send_audio_frame(pcm, HOP_LEN_SAMPLES, /*is_final=*/false);
        vTaskDelay(pdMS_TO_TICKS(4));   // ~4-5× real time (20ms hop / ~4-5)

        uint32_t notify;
        if (xTaskNotifyWait(0, ULONG_MAX, &notify, 0) == pdTRUE &&
            (notify & NOTIFY_BIT_END_OF_UTT)) {
            goto close_stream;
        }
    }

    // Phase 2: live handoff — Core 0 sends NOTIFY_BIT_TRIGGER for each new
    // hop written during STREAMING (see kws_task in app_main.cpp). Advance
    // read_ptr by exactly one hop per wakeup to avoid skipping frames.
    for (;;) {
        uint32_t notify;
        xTaskNotifyWait(0, ULONG_MAX, &notify, portMAX_DELAY);
        if (notify & NOTIFY_BIT_END_OF_UTT) break;

        // Drain all hops that have accumulated since last wakeup.
        while (read_ptr != g_ring_buf.write_ptr) {
            int16_t pcm[HOP_LEN_SAMPLES];
            for (uint32_t i = 0; i < HOP_LEN_SAMPLES; ++i) {
                uint32_t off = (read_ptr + i * BYTES_PER_SAMPLE) % RING_BUFFER_BYTES;
                memcpy(&pcm[i], &g_ring_buf.data[off], sizeof(int16_t));
            }
            read_ptr = (read_ptr + HOP_LEN_BYTES) % RING_BUFFER_BYTES;
            send_audio_frame(pcm, HOP_LEN_SAMPLES, /*is_final=*/false);
        }
    }

close_stream:
    // Step 6.7 stream closure
    int16_t silence[HOP_LEN_SAMPLES] = {0};
    send_audio_frame(silence, HOP_LEN_SAMPLES, /*is_final=*/true);  // marker bit
    ESP_LOGI(TAG, "stream closed, awaiting server response");
}

// ---------------------------------------------------------------------
// Step 7 — receive + decrypt + execute + ACK
// ---------------------------------------------------------------------
static void handle_server_response()
{
    g_system_state = SystemState::AWAITING_RESPONSE;

    uint8_t rx_buf[256];
    struct timeval tv{ .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = recv(s_udp_sock, rx_buf, sizeof(rx_buf), 0);
    if (n <= (int)(12 + 16)) { ESP_LOGW(TAG, "no/short response"); goto rearm; }

    {
        uint8_t* nonce = rx_buf;
        uint8_t* ct    = rx_buf + 12;
        size_t   ct_len = n - 12 - 16;
        uint8_t* tag   = rx_buf + n - 16;
        uint8_t  plain[256];

        int rc = mbedtls_gcm_auth_decrypt(&s_gcm_ctx, ct_len, nonce, 12,
                                           nullptr, 0, tag, 16, ct, plain);
        if (rc != 0) { ESP_LOGW(TAG, "auth failed, dropping forged/corrupt packet"); goto rearm; }

        plain[ct_len] = '\0';
        cJSON* json = cJSON_Parse((char*)plain);
        if (json) {
            int pin   = cJSON_GetObjectItem(json, "pin")->valueint;
            int state = cJSON_GetObjectItem(json, "state")->valueint;
            // TODO: gpio_set_level((gpio_num_t)pin, state);
            ESP_LOGI(TAG, "action: GPIO %d -> %d", pin, state);
            cJSON_Delete(json);
        }

        uint8_t ack = 0x01;
        sendto(s_udp_sock, &ack, 1, 0,
               (struct sockaddr*)&s_server_addr, sizeof(s_server_addr));
    }

rearm:
    // Step 7.4 — re-arm KWS, leave no residual state.
    kws_model_reset();
    vad_reset();
    g_system_state = SystemState::IDLE_KWS;
}

// ---------------------------------------------------------------------
void net_task(void* /*pvParameters*/)
{
    net_wifi_and_socket_init();

    for (;;) {
        uint32_t notify;
        // Idle: send NAT keep-alives while waiting for a trigger from Core 0.
        if (xTaskNotifyWait(0, ULONG_MAX, &notify, pdMS_TO_TICKS(1000)) == pdTRUE
            && (notify & NOTIFY_BIT_TRIGGER)) {
            stream_utterance(g_trigger_msg.read_ptr_bytes);
            handle_server_response();
        } else {
            send_heartbeat_if_due();
        }
    }
}
