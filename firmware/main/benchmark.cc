// Panel timing benchmark.
//
// Enabled with CONFIG_COUNTER_PANEL_BENCHMARK. It runs once at boot, before
// the UI takes over, so refresh costs can be measured without anyone having to
// stand at the device pressing buttons.
//
// What it answers: how much of a refresh is fixed overhead and how much scales
// with the height of the rectangle. That decides how fast the clock can tick
// and whether a flashing background is affordable.

#include "benchmark.h"

#include <cstring>

#include "canvas.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "bench";

// Heights spanning what the UI actually asks for: a status line, the clock
// band, the clock plus bar, and a whole-screen inversion.
constexpr int kHeights[] = {20, 60, 131, 159, 300};

int64_t TimeFull(zectrix_epd_handle_t epd, Canvas* canvas, bool black) {
    canvas->Clear(black);
    const int64_t start = esp_timer_get_time();
    zectrix_epd_refresh_full_1bpp(epd, canvas->data(), Canvas::size());
    return (esp_timer_get_time() - start) / 1000;
}

void TimePartials(zectrix_epd_handle_t epd, Canvas* canvas, uint8_t* patch) {
    for (int height : kHeights) {
        int64_t total = 0;
        int runs = 0;
        constexpr int kRuns = 3;
        for (int run = 0; run < kRuns; ++run) {
            // Alternate the patch so every run actually moves pixels; an
            // unchanged rectangle would not exercise the waveform.
            const int y = height >= Canvas::kHeight ? 0 : 10;
            const int h = height > Canvas::kHeight - y ? Canvas::kHeight - y
                                                       : height;
            canvas->FillRect(0, y, Canvas::kWidth, h, run % 2 == 0);
            canvas->ExtractRect(0, y, Canvas::kWidth, h, patch);

            const zectrix_epd_rect_t rect = {0, y, Canvas::kWidth, h};
            const int64_t start = esp_timer_get_time();
            const esp_err_t err = zectrix_epd_refresh_partial_1bpp(
                epd, &rect, patch, static_cast<size_t>(Canvas::kWidth / 8) * h);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "partial h=%d failed: %s", h,
                         esp_err_to_name(err));
                break;
            }
            total += (esp_timer_get_time() - start) / 1000;
            ++runs;
        }
        if (runs > 0) {
            ESP_LOGI(kTag, "partial h=%3d avg = %lldms", height, total / runs);
        }
        TimeFull(epd, canvas, false);  // clean slate for the next height
    }
}

}  // namespace

void PanelBenchmark(zectrix_epd_handle_t epd) {
    static Canvas canvas;
    static uint8_t patch[ZECTRIX_EPD_1BPP_FRAME_BYTES];

    ESP_LOGI(kTag, "=== panel benchmark ===");

    for (int i = 0; i < 2; ++i) {
        ESP_LOGI(kTag, "full(%s) = %lldms", i % 2 == 0 ? "white" : "black",
                 TimeFull(epd, &canvas, i % 2 != 0));
    }
    TimeFull(epd, &canvas, false);

    TimePartials(epd, &canvas, patch);

    ESP_LOGI(kTag, "=== benchmark done ===");
}
