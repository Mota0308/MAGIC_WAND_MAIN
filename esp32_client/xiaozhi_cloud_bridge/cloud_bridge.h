/**
 * Optional cloud bridge for xiaozhi-esp32: POST events to Magic Wand Railway backend.
 * Copy this and cloud_bridge.cc into xiaozhi-esp32 main/ and add to CMakeLists + Kconfig.
 */
#ifndef CLOUD_BRIDGE_H
#define CLOUD_BRIDGE_H

#include <string>

namespace CloudBridge {

/** Call once at startup (e.g. after WiFi connected). No-op if CONFIG_MAGIC_WAND_CLOUD_BRIDGE is disabled. */
void Init();

/**
 * Send an event to the configured cloud URL (non-blocking; queues and sends in background).
 * label: e.g. "stt", "tts", "wake"
 * text: optional payload (e.g. user speech or TTS sentence)
 */
void SendEvent(const char* label, const char* text = nullptr);

}  // namespace CloudBridge

#endif  // CLOUD_BRIDGE_H
