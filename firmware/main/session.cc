#include "session.h"

#include <algorithm>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "session";
constexpr char kNamespace[] = "counter";
constexpr char kKeyConfig[] = "config";

// Guards against a runaway catch-up loop if the clock ever jumps forward.
constexpr int kMaxCatchUpPhases = 100;

}  // namespace

int64_t Session::PhaseDuration(Phase phase) const {
    switch (phase) {
        case Phase::kTalk:
            return static_cast<int64_t>(config_.talk_min) * 60 * 1000;
        case Phase::kAlarm:
            return kAlarmMs;
        case Phase::kBreak:
            return static_cast<int64_t>(config_.break_sec) * 1000;
    }
    return 0;
}

void Session::EnterPhase(Phase phase, int talk_index, int64_t at) {
    ++epoch_;
    running_ = true;
    paused_at_ = 0;
    phase_ = phase;
    talk_index_ = talk_index;
    phase_start_ = at;
    phase_end_ = at + PhaseDuration(phase);
}

void Session::Start(int64_t now) { EnterPhase(Phase::kTalk, 0, now); }

void Session::Stop() {
    running_ = false;
    paused_at_ = 0;
}

void Session::Finish() { Stop(); }

void Session::NextAfterTalk(int64_t at) {
    if (config_.break_sec > 0) {
        EnterPhase(Phase::kBreak, talk_index_, at);
    } else {
        EnterPhase(Phase::kTalk, talk_index_ + 1, at);
    }
}

void Session::Advance(int64_t at, bool skipped) {
    switch (phase_) {
        case Phase::kTalk:
            // Letting a talk expire sounds the buzzer; skipping steps over it.
            if (!skipped) {
                EnterPhase(Phase::kAlarm, talk_index_, at);
            } else if (last_talk()) {
                Finish();
            } else {
                NextAfterTalk(at);
            }
            return;
        case Phase::kAlarm:
            if (last_talk()) {
                Finish();
            } else {
                NextAfterTalk(at);
            }
            return;
        case Phase::kBreak:
            EnterPhase(Phase::kTalk, talk_index_ + 1, at);
            return;
    }
}

bool Session::Tick(int64_t now) {
    if (!running_ || paused()) return false;

    bool changed = false;
    int guard = 0;
    while (running_ && now >= phase_end_ && guard++ < kMaxCatchUpPhases) {
        Advance(phase_end_, false);
        changed = true;
    }
    return changed;
}

void Session::Next(int64_t now) {
    if (!running_) return;
    paused_at_ = 0;  // moving on implies carrying on
    Advance(now, true);
}

void Session::RestartSegment(int64_t now) {
    if (!running_) return;
    // The alarm belongs to the talk that just ended, so restarting during it
    // restarts that talk rather than the buzzer.
    const Phase target = phase_ == Phase::kBreak ? Phase::kBreak : Phase::kTalk;
    EnterPhase(target, talk_index_, now);
}

void Session::PreviousSegment(int64_t now) {
    if (!running_) return;

    if (phase_ == Phase::kBreak) {
        EnterPhase(Phase::kTalk, talk_index_, now);
        return;
    }

    // Talk (or its alarm): step back over the break that preceded it, if any.
    if (talk_index_ <= 0) {
        EnterPhase(Phase::kTalk, 0, now);
    } else if (config_.break_sec > 0) {
        EnterPhase(Phase::kBreak, talk_index_ - 1, now);
    } else {
        EnterPhase(Phase::kTalk, talk_index_ - 1, now);
    }
}

void Session::SetPaused(bool paused, int64_t now) {
    if (!running_ || paused == this->paused()) return;
    if (paused) {
        paused_at_ = now;
    } else {
        // Slide the whole phase forward by however long we sat paused.
        const int64_t held = now - paused_at_;
        phase_start_ += held;
        phase_end_ += held;
        paused_at_ = 0;
    }
}

int64_t Session::remaining_ms(int64_t now) const {
    if (!running_) return 0;
    const int64_t reference = paused() ? paused_at_ : now;
    return std::max<int64_t>(0, phase_end_ - reference);
}

bool Session::warning(int64_t now) const {
    if (!running_ || phase_ != Phase::kTalk || config_.warn_sec <= 0) {
        return false;
    }
    const int64_t warn_ms = static_cast<int64_t>(config_.warn_sec) * 1000;
    // A warning as long as the talk would fire the instant it starts, which is
    // no warning at all. The web control drops it the same way.
    if (warn_ms >= PhaseDuration(Phase::kTalk)) return false;
    return remaining_ms(now) <= warn_ms;
}

/* ------------------------------------------------------------------ *
 * Persistence
 * ------------------------------------------------------------------ */

void SessionConfigLoad(SessionConfig* config) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;

    SessionConfig stored;
    size_t length = sizeof(stored);
    const esp_err_t err = nvs_get_blob(handle, kKeyConfig, &stored, &length);
    nvs_close(handle);

    if (err == ESP_OK && length == sizeof(stored)) {
        *config = stored;
        ESP_LOGI(kTag, "config loaded: %d talks, %d min, warn %ds, break %ds",
                 config->talks, config->talk_min, config->warn_sec,
                 config->break_sec);
    }
}

void SessionConfigSave(const SessionConfig& config) {
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, kKeyConfig, &config, sizeof(config)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}
