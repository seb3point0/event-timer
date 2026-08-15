#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "display.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "font.h"
#include "fonts/font_clock.h"
#include "fonts/font_med.h"
#include "fonts/font_small.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "session.h"
#include "sound.h"
#include "zectrix_board.h"

namespace {

constexpr char kTag[] = "ui";

int64_t NowMs() { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------ *
 * Layout
 *
 * The panel is 400x300 and there is exactly one of it, so everything is
 * placed in absolute pixels.
 * ------------------------------------------------------------------ */

constexpr int kMargin = 12;
constexpr int kHeaderBaseline = 20;
constexpr int kRuleY = 30;

constexpr int kRowHeight = 44;
constexpr int kRowTop = 38;
constexpr int kStartRowTop = 222;
constexpr int kStartRowHeight = 46;

// Measured on this panel: a full refresh is 1247ms and a partial is 808ms
// almost regardless of size, so what costs time is the *number* of refreshes,
// not their area. The bar therefore sits directly under the digits -- close
// enough that the two land in one dirty band and cost one refresh between
// them. Moving it to the foot of the screen doubled the per-tick cost.
constexpr int kTimerLabelBaseline = 40;
constexpr int kClockBaseline = 200;
constexpr int kBarX = 16;
constexpr int kBarY = 211;
constexpr int kBarW = Canvas::kWidth - 2 * kBarX;
constexpr int kBarH = 18;
constexpr int kStatusBaseline = 272;

// Pause glyph in the top right: two bars, drawn rather than typeset so it
// reads as a symbol at a distance instead of as a word to be parsed.
constexpr int kPauseY = 14;
constexpr int kPauseH = 30;
constexpr int kPauseW = 24;
constexpr int kPauseBar = 9;

// Rows of the setup screen, in the order they are shown.
enum Row {
    kRowTalks = 0,
    kRowLength,
    kRowWarn,
    kRowBreak,
    kRowStart,
    kRowCount,
};

constexpr int kEditableRows = kRowStart;

struct RowSpec {
    int step;
    int min;
    int max;
};

// One press, one step. The panel needs a few hundred milliseconds per refresh,
// so single-second steps would be unusable here: these are the coarsest steps
// that still reach the values an MC actually wants.
constexpr RowSpec kRowSpecs[kEditableRows] = {
    {1, 1, 20},     // talks
    {1, 1, 180},    // talk length, minutes
    {15, 0, 600},   // warning, seconds
    {15, 0, 1800},  // break, seconds
};

const char* const kRowLabels[kRowCount] = {
    "TALKS", "DURATION", "WARNING", "BREAK", "START SESSION",
};

int* RowValue(SessionConfig* config, int row) {
    switch (row) {
        case kRowTalks: return &config->talks;
        case kRowLength: return &config->talk_min;
        case kRowWarn: return &config->warn_sec;
        case kRowBreak: return &config->break_sec;
        default: return nullptr;
    }
}

void FormatRowValue(const SessionConfig& config, int row, char* out, size_t n) {
    switch (row) {
        case kRowTalks:
            std::snprintf(out, n, "%d", config.talks);
            return;
        case kRowLength:
            std::snprintf(out, n, "%d MIN", config.talk_min);
            return;
        case kRowWarn:
            if (config.warn_sec == 0) {
                std::snprintf(out, n, "OFF");
            } else {
                std::snprintf(out, n, "%d S", config.warn_sec);
            }
            return;
        case kRowBreak:
            if (config.break_sec == 0) {
                std::snprintf(out, n, "NONE");
            } else {
                std::snprintf(out, n, "%d S", config.break_sec);
            }
            return;
        default:
            if (n > 0) out[0] = '\0';
            return;
    }
}

int RowTop(int row) {
    return row == kRowStart ? kStartRowTop : kRowTop + row * kRowHeight;
}

int RowHeight(int row) {
    return row == kRowStart ? kStartRowHeight : kRowHeight;
}

void Adjust(SessionConfig* config, int row, int direction) {
    if (row < 0 || row >= kEditableRows) return;
    int* value = RowValue(config, row);
    if (value == nullptr) return;
    const RowSpec& spec = kRowSpecs[row];
    *value = std::clamp(*value + direction * spec.step, spec.min, spec.max);
}

void FormatClock(int64_t ms, char* out, size_t n) {
    // Ceil, so 00:00 only appears once the phase is genuinely over.
    int64_t seconds = (ms + 999) / 1000;
    if (seconds < 0) seconds = 0;
    if (seconds > 99 * 60 + 59) seconds = 99 * 60 + 59;
    std::snprintf(out, n, "%02d:%02d", static_cast<int>(seconds / 60),
                  static_cast<int>(seconds % 60));
}

/* ------------------------------------------------------------------ *
 * Screens
 * ------------------------------------------------------------------ */

void RenderSetup(Canvas* canvas, const SessionConfig& config, int selected,
                 bool editing, const ZectrixPowerSnapshot& power) {
    canvas->Clear(false);

    canvas->Text(kMargin + 2, kHeaderBaseline, zt_font_small, "SETUP", true);
    if (power.battery_valid) {
        char battery[16];
        std::snprintf(battery, sizeof(battery), "%u%%",
                      static_cast<unsigned>(power.battery_percent));
        const int width = canvas->TextWidth(zt_font_small, battery);
        canvas->Text(Canvas::kWidth - kMargin - 2 - width, kHeaderBaseline,
                     zt_font_small, battery, true);
    }
    canvas->FillRect(kMargin, kRuleY, Canvas::kWidth - 2 * kMargin, 2, true);

    for (int row = 0; row < kRowCount; ++row) {
        const int top = RowTop(row);
        const int height = RowHeight(row);
        const bool active = row == selected;
        // Editing fills the row; so does landing on Start, which makes the one
        // row that does something on OK look like a button.
        const bool filled = active && (editing || row == kRowStart);
        const bool ink = !filled;

        if (filled) {
            canvas->FillRect(kMargin, top, Canvas::kWidth - 2 * kMargin, height,
                             true);
        } else if (active) {
            canvas->StrokeRect(kMargin, top, Canvas::kWidth - 2 * kMargin,
                               height, 3, true);
        }

        const int baseline = top + (height + 21) / 2;
        if (row == kRowStart) {
            canvas->TextCentered(Canvas::kWidth / 2, baseline, zt_font_med,
                                 kRowLabels[row], ink);
            continue;
        }

        canvas->Text(kMargin + 12, baseline, zt_font_med, kRowLabels[row], ink);

        char value[24];
        FormatRowValue(config, row, value, sizeof(value));
        char shown[32];
        // The chevrons are the only cue that up/down now change the number
        // instead of moving the highlight.
        if (active && editing) {
            std::snprintf(shown, sizeof(shown), "< %s >", value);
        } else {
            std::snprintf(shown, sizeof(shown), "%s", value);
        }
        const int width = canvas->TextWidth(zt_font_med, shown);
        canvas->Text(Canvas::kWidth - kMargin - 12 - width, baseline,
                     zt_font_med, shown, ink);
    }
}

void RenderTimer(Canvas* canvas, const Session& session, int64_t now,
                 bool flash) {
    const bool warning = session.warning(now);
    const bool alarm = session.phase() == Phase::kAlarm;
    const bool is_break = session.phase() == Phase::kBreak;
    // Wrap-up and the end buzzer alternate the whole panel between black and
    // white on every tick. A whole-screen partial refresh is 917ms, so this
    // rides along with the once-a-second clock update at no extra cost. A
    // break never flashes: nobody is on the clock, so nothing needs urgency.
    const bool inverted = (warning || alarm) && flash;
    const bool ink = !inverted;

    canvas->Clear(inverted);

    // A break sits on grey, so a glance from across the room tells you nobody
    // is on the clock without having to read anything.
    if (is_break) {
        canvas->FillGray(0, 0, Canvas::kWidth, Canvas::kHeight,
                         Canvas::Gray::kMid);
    }

    char label[40];
    switch (session.phase()) {
        case Phase::kBreak:
            std::snprintf(label, sizeof(label), "NEXT TALK %d",
                          session.talk_index() + 2);
            break;
        case Phase::kAlarm:
            std::snprintf(label, sizeof(label), "TIME IS UP");
            break;
        case Phase::kTalk:
        default:
            std::snprintf(label, sizeof(label), "TALK %d OF %d",
                          session.talk_index() + 1, session.config().talks);
            break;
    }
    canvas->TextCentered(Canvas::kWidth / 2, kTimerLabelBaseline, zt_font_med,
                         label, ink);

    char clock[16];
    FormatClock(session.remaining_ms(now), clock, sizeof(clock));
    canvas->TextCentered(Canvas::kWidth / 2, kClockBaseline, zt_font_clock,
                         clock, ink);

    // The bar fills left to right as the phase is consumed. It moves in 4px
    // steps so that most seconds change only the digits: a bar creeping by one
    // pixel a second would dirty a second region of the panel every tick and
    // double the refresh cost for no visible gain.
    const int64_t length = std::max<int64_t>(1, session.phase_length_ms());
    const int64_t elapsed =
        std::max<int64_t>(0, length - session.remaining_ms(now));
    const int filled =
        static_cast<int>((elapsed * (kBarW - 6)) / length) / 4 * 4;
    canvas->StrokeRect(kBarX, kBarY, kBarW, kBarH, 2, ink);
    if (filled > 0) {
        canvas->FillRect(kBarX + 3, kBarY + 3, filled, kBarH - 6, ink);
    }

    if (session.paused()) {
        // Two bars in the top right corner: the one piece of state that has to
        // be readable at a glance from anywhere in the room.
        const int x = Canvas::kWidth - kMargin - kPauseW;
        canvas->FillRect(x, kPauseY, kPauseBar, kPauseH, ink);
        canvas->FillRect(x + kPauseW - kPauseBar, kPauseY, kPauseBar, kPauseH,
                         ink);
        canvas->TextCentered(Canvas::kWidth / 2, kStatusBaseline, zt_font_med,
                             "PAUSED", ink);
    } else if (warning) {
        canvas->TextCentered(Canvas::kWidth / 2, kStatusBaseline, zt_font_med,
                             "WRAP UP", ink);
    }
}

void RenderComplete(Canvas* canvas) {
    canvas->Clear(false);
    canvas->TextCentered(Canvas::kWidth / 2, 150, zt_font_med,
                         "SESSION COMPLETE", true);
    canvas->TextCentered(Canvas::kWidth / 2, 196, zt_font_small,
                         "PRESS ANY BUTTON", true);
}

/* ------------------------------------------------------------------ *
 * Timing
 * ------------------------------------------------------------------ */

// How long the loop may sleep before the visible clock would go stale.
TickType_t NextRedrawDelay(const Session& session, int64_t now) {
    if (!session.running() || session.paused()) return portMAX_DELAY;

    // Every refresh the timer needs fits inside a second: 831ms for the clock
    // band, 917ms even for a whole-screen inversion. Only a full refresh
    // (1219ms) does not, which is why one never happens mid-phase.
    const int64_t remaining = session.remaining_ms(now);
    const int64_t step = 1000;
    int64_t delay = remaining - ((remaining - 1) / step) * step;

    // Land exactly on the wrap-up threshold so the inversion is not late.
    const int64_t warn_ms =
        static_cast<int64_t>(session.config().warn_sec) * 1000;
    if (session.phase() == Phase::kTalk && warn_ms > 0 && remaining > warn_ms) {
        delay = std::min(delay, remaining - warn_ms);
    }

    return pdMS_TO_TICKS(std::max<int64_t>(1, delay));
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

struct UiState {
    Session session;
    SessionConfig config;
    bool in_timer = false;
    int selected = kRowTalks;
    bool editing = false;
    // A back press waits this long to see whether a second one follows.
    int64_t back_pending_at = 0;
};

constexpr int64_t kDoubleClickMs = 350;

void HandleSetupEvent(const ZectrixButtonEvent& event, UiState* ui,
                      int64_t now) {
    const bool click = event.action == ZectrixButtonAction::kClick;

    switch (event.button) {
        case ZectrixButton::kUp:
            if (!click) return;
            if (ui->editing) {
                Adjust(&ui->config, ui->selected, +1);
            } else {
                ui->selected = (ui->selected + kRowCount - 1) % kRowCount;
            }
            return;

        case ZectrixButton::kDown:
            if (!click) return;
            if (ui->editing) {
                Adjust(&ui->config, ui->selected, -1);
            } else {
                ui->selected = (ui->selected + 1) % kRowCount;
            }
            return;

        case ZectrixButton::kOk:
            if (!click) {  // held: back out of edit mode
                ui->editing = false;
                return;
            }
            if (ui->selected == kRowStart) {
                SessionConfigSave(ui->config);
                ui->session.set_config(ui->config);
                ui->session.Start(now);
                ui->in_timer = true;
                return;
            }
            ui->editing = !ui->editing;
            if (!ui->editing) SessionConfigSave(ui->config);
            return;
    }
}

void HandleTimerEvent(const ZectrixButtonEvent& event, UiState* ui,
                      int64_t now) {
    const bool click = event.action == ZectrixButtonAction::kClick;

    switch (event.button) {
        case ZectrixButton::kOk:
            if (click) {
                ui->session.TogglePause(now);
            } else {
                // Held for two seconds: end the session and go back to setup.
                SoundStop();
                ui->session.Stop();
                ui->in_timer = false;
                ui->selected = kRowStart;
                ui->editing = false;
            }
            return;

        case ZectrixButton::kUp:
            // Forward a segment, jumping past the end buzzer.
            if (click) ui->session.Next(now);
            return;

        case ZectrixButton::kDown:
            // Resolved by the double-click window back in the main loop:
            // one press restarts this segment, two jump to the previous one.
            if (click) ui->back_pending_at = now == 0 ? 1 : now;
            return;
    }
}

/* ------------------------------------------------------------------ *
 * Shutdown
 * ------------------------------------------------------------------ */

[[noreturn]] void PowerOff(ZectrixBoard* board, Display* display,
                           Canvas* canvas) {
    ESP_LOGI(kTag, "powering off");
    SoundStop();
    canvas->Clear(false);
    display->ShowFull(*canvas);
    board->SetPowerLed(false);
    board->SetAudioPower(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    board->CutBatteryPower();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

}  // namespace

/* ------------------------------------------------------------------ *
 * Main loop
 * ------------------------------------------------------------------ */

void UiRun(ZectrixBoard* board, zectrix_epd_handle_t epd) {
    // Both are far too large to sit on a task stack.
    static Canvas canvas;
    static Display display;
    display.Begin(epd);

    UiState ui;
    SessionConfigLoad(&ui.config);
    ui.session.set_config(ui.config);

#if CONFIG_COUNTER_AUTOSTART
    // Compressed session so a whole run fits in a serial capture.
    ui.config = {2, 1, 15, 30};
    ui.session.set_config(ui.config);
    ui.session.Start(NowMs());
    ui.in_timer = true;
#endif

    // One-shot cue bookkeeping, keyed on the phase epoch: re-entering a segment
    // sounds again, a plain redraw does not.
    uint32_t chimed_epoch = 0;
    uint32_t warned_epoch = 0;
    uint32_t buzzed_epoch = 0;
    bool flash = false;
    uint32_t drawn_epoch = 0;

    ZectrixPowerSnapshot power = board->ReadPowerSnapshot();
    int64_t power_read_at = NowMs();

    bool was_in_timer = false;
    RenderSetup(&canvas, ui.config, ui.selected, ui.editing, power);
    display.ShowFull(canvas);

    while (true) {
        int64_t now = NowMs();

        // Setup has nothing animating, but it still wakes periodically so the
        // battery reading does not sit frozen at whatever it was on boot. An
        // unchanged frame diffs to nothing and costs no refresh.
        TickType_t wait = ui.in_timer ? NextRedrawDelay(ui.session, now)
                                      : pdMS_TO_TICKS(30000);
        if (ui.back_pending_at != 0) {
            const int64_t left = kDoubleClickMs - (now - ui.back_pending_at);
            wait = std::min<TickType_t>(
                wait, pdMS_TO_TICKS(std::max<int64_t>(1, left)));
        }

        ZectrixButtonEvent event;
        bool got_event = board->WaitButton(&event, wait);
        now = NowMs();

        // Resolve any pending back press before anything else looks at input.
        if (ui.back_pending_at != 0) {
            const bool second_click =
                got_event && event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kClick;
            if (second_click) {
                ui.session.PreviousSegment(now);
                ui.back_pending_at = 0;
                got_event = false;  // consumed by the double click
            } else if (now - ui.back_pending_at >= kDoubleClickMs) {
                ui.session.RestartSegment(now);
                ui.back_pending_at = 0;
            }
        }

        if (got_event) {
            // Holding the power button wins everywhere.
            if (event.button == ZectrixButton::kDown &&
                event.action == ZectrixButtonAction::kLongPress) {
                PowerOff(board, &display, &canvas);
            }
            if (ui.in_timer) {
                HandleTimerEvent(event, &ui, now);
            } else {
                HandleSetupEvent(event, &ui, now);
            }
        }

        if (ui.in_timer) ui.session.Tick(now);

        // A session that reached its end shows a card and drops back to setup.
        if (ui.in_timer && !ui.session.running()) {
            RenderComplete(&canvas);
            display.ShowFull(canvas);
            board->DrainButtons();
            ZectrixButtonEvent dismiss;
            board->WaitButton(&dismiss, pdMS_TO_TICKS(8000));
            ui.in_timer = false;
            ui.selected = kRowStart;
            ui.editing = false;
            ui.back_pending_at = 0;
            board->DrainButtons();
        }

        if (ui.in_timer) {
            const uint32_t epoch = ui.session.epoch();
            if (ui.session.phase() == Phase::kTalk && chimed_epoch != epoch) {
                chimed_epoch = epoch;
                SoundPlay(Cue::kChime);
            }
            if (ui.session.warning(now) && warned_epoch != epoch) {
                warned_epoch = epoch;
                SoundPlay(Cue::kChime);
            }
            if (ui.session.phase() == Phase::kAlarm && buzzed_epoch != epoch) {
                buzzed_epoch = epoch;
                SoundPlay(Cue::kAlert, 3);
            }

            // Wrap-up and the buzzer alternate the background every tick;
            // everything else sits still.
            const bool wants_flash =
                !ui.session.paused() &&
                (ui.session.warning(now) ||
                 ui.session.phase() == Phase::kAlarm);
            flash = wants_flash ? !flash : false;

            RenderTimer(&canvas, ui.session, now, flash);
        } else {
            if (now - power_read_at > 30000) {
                power = board->ReadPowerSnapshot();
                power_read_at = now;
            }
            RenderSetup(&canvas, ui.config, ui.selected, ui.editing, power);
        }

        // Swapping screens is worth a clean full refresh; within a screen the
        // display layer works out how little it can get away with.
        // Full refreshes are a 1.2s black flash, so they are confined to the
        // two moments that are already a visual break: changing screen, and
        // changing phase. Those also serve as the ghosting reset, which is why
        // nothing else during a phase is allowed to promote itself to one.
        const bool screen_changed = ui.in_timer != was_in_timer;
        const bool phase_changed =
            ui.in_timer && ui.session.running() &&
            ui.session.epoch() != drawn_epoch;

        was_in_timer = ui.in_timer;
        if (ui.in_timer && ui.session.running()) drawn_epoch = ui.session.epoch();

        if (screen_changed || phase_changed) {
            display.ShowFull(canvas);
        } else {
            display.Show(canvas, ui.in_timer && ui.session.running());
        }
    }
}
