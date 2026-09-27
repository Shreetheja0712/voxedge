#pragma once

// Core 1 entry point. Handles:
//   Step 5 — Wi-Fi + UDP socket + AES-GCM init, NAT keep-alive
//   Step 6 — catch-up burst + live stream on trigger, VAD-driven close
//   Step 7 — response decrypt/parse/GPIO action/ACK, then re-arm KWS
// Pinned to core 1 at lower priority than the Core-0 KWS task.
void net_task(void* pvParameters);
