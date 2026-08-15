#ifndef ZT_SESSION_H_
#define ZT_SESSION_H_

#include <stdint.h>

// Phases run talk -> alarm -> break -> talk ... The alarm is the short buzzer
// window at the end of a talk; skipping forward jumps past it, letting it
// expire plays it.
enum class Phase : uint8_t {
    kTalk = 0,
    kAlarm,
    kBreak,
};

struct SessionConfig {
    int talks = 3;
    int talk_min = 10;
    int warn_sec = 60;
    int break_sec = 60;
};

// How long the end-of-talk buzzer window lasts.
inline constexpr int64_t kAlarmMs = 3000;

// A segment is what the back button navigates between: one talk (its alarm
// included) or one break. Talks and breaks both carry the index of the talk
// they belong to.
class Session {
public:
    void set_config(const SessionConfig& config) { config_ = config; }
    const SessionConfig& config() const { return config_; }

    void Start(int64_t now);
    void Stop();

    // Catches up on any phases whose end time has already passed. Returns true
    // when something changed, so the caller knows to redraw.
    bool Tick(int64_t now);

    // Front button: end the current segment early and move on, skipping the
    // buzzer. Mirrors the web control's "skip".
    void Next(int64_t now);
    // Back button: restart the segment we are in.
    void RestartSegment(int64_t now);
    // Back button, double click: jump to the start of the previous segment.
    void PreviousSegment(int64_t now);
    void SetPaused(bool paused, int64_t now);
    void TogglePause(int64_t now) { SetPaused(!paused(), now); }

    bool running() const { return running_; }
    bool paused() const { return paused_at_ != 0; }
    Phase phase() const { return phase_; }
    int talk_index() const { return talk_index_; }
    // Bumped every time a phase begins, including a restart of the same one.
    // One-shot cues key off this, so re-entering a talk chimes again but a
    // redraw within the same talk does not.
    uint32_t epoch() const { return epoch_; }

    int64_t remaining_ms(int64_t now) const;
    int64_t phase_length_ms() const { return phase_end_ - phase_start_; }
    // True once the countdown has crossed into the wrap-up window.
    bool warning(int64_t now) const;

private:
    void EnterPhase(Phase phase, int talk_index, int64_t at);
    void Advance(int64_t at, bool skipped);
    void NextAfterTalk(int64_t at);
    void Finish();
    int64_t PhaseDuration(Phase phase) const;
    bool last_talk() const { return talk_index_ >= config_.talks - 1; }

    SessionConfig config_;
    bool running_ = false;
    Phase phase_ = Phase::kTalk;
    int talk_index_ = 0;
    int64_t phase_start_ = 0;
    int64_t phase_end_ = 0;
    int64_t paused_at_ = 0;  // 0 means running
    uint32_t epoch_ = 0;
};

// Config is kept in NVS so the box comes back with the last event's settings.
void SessionConfigLoad(SessionConfig* config);
void SessionConfigSave(const SessionConfig& config);

#endif  // ZT_SESSION_H_
