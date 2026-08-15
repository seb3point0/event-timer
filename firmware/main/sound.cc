#include "sound.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "audio_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"

// Headerless 16 kHz mono s16le, produced by tools/prep_audio.sh. There is no
// decoder on the device; these bytes go straight to I2S.
extern const uint8_t chime_pcm_start[] asm("_binary_chime_16k_pcm_start");
extern const uint8_t chime_pcm_end[] asm("_binary_chime_16k_pcm_end");
extern const uint8_t alert_pcm_start[] asm("_binary_alert_16k_pcm_start");
extern const uint8_t alert_pcm_end[] asm("_binary_alert_16k_pcm_end");

namespace {

constexpr char kTag[] = "sound";

// Small enough that a stop request lands within ~30ms, large enough to keep the
// I2S DMA queue comfortably ahead of the codec.
constexpr size_t kChunkSamples = 512;

constexpr int kVolume = 100;  // the onboard speaker needs every dB it can get

struct Request {
    Cue cue;
    int repeats;
};

ZectrixBoard* g_board = nullptr;
QueueHandle_t g_queue = nullptr;
AudioCodec* g_codec = nullptr;
std::atomic<uint32_t> g_stop_token{0};

const int16_t* CueSamples(Cue cue, size_t* count) {
    const uint8_t* start = cue == Cue::kChime ? chime_pcm_start : alert_pcm_start;
    const uint8_t* end = cue == Cue::kChime ? chime_pcm_end : alert_pcm_end;
    *count = static_cast<size_t>(end - start) / sizeof(int16_t);
    return reinterpret_cast<const int16_t*>(start);
}

// The amplifier stays off until something actually needs to make a noise.
bool EnsureCodec() {
    if (g_codec != nullptr) return true;
    if (g_board == nullptr) return false;

    g_board->SetAudioPower(true);
    // The rail needs a moment to settle before the codec is addressed over I2C.
    vTaskDelay(pdMS_TO_TICKS(50));

    g_codec = g_board->PrepareAudio();
    if (g_codec == nullptr) {
        ESP_LOGE(kTag, "codec unavailable");
        return false;
    }
    g_codec->SetOutputVolume(kVolume);
    return true;
}

void PlayTask(void*) {
    std::vector<int16_t> chunk(kChunkSamples);

    while (true) {
        Request request;
        if (xQueueReceive(g_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        if (!EnsureCodec()) continue;

        const uint32_t token = g_stop_token.load();
        size_t total = 0;
        const int16_t* samples = CueSamples(request.cue, &total);

        for (int pass = 0; pass < request.repeats; ++pass) {
            for (size_t offset = 0; offset < total; offset += kChunkSamples) {
                // A stop, or a newer cue arriving, abandons the rest of this one.
                if (g_stop_token.load() != token) break;
                if (uxQueueMessagesWaiting(g_queue) > 0) break;

                const size_t n = std::min(kChunkSamples, total - offset);
                chunk.assign(samples + offset, samples + offset + n);
                g_codec->OutputData(chunk);
            }
            if (g_stop_token.load() != token) break;
        }
    }
}

}  // namespace

void SoundInit(ZectrixBoard* board) {
    g_board = board;
    g_queue = xQueueCreate(4, sizeof(Request));
    xTaskCreate(PlayTask, "sound", 4096, nullptr, 5, nullptr);
}

void SoundPlay(Cue cue, int repeats) {
    if (g_queue == nullptr) return;
    const Request request = {cue, repeats < 1 ? 1 : repeats};
    xQueueSend(g_queue, &request, 0);
}

void SoundStop() {
    if (g_queue == nullptr) return;
    xQueueReset(g_queue);
    g_stop_token.fetch_add(1);
}
