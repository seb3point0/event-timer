#include "display.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "display";

}  // namespace

esp_err_t Display::Begin(zectrix_epd_handle_t epd) {
    epd_ = epd;
    shown_valid_ = false;
    partials_since_full_ = 0;
    return ESP_OK;
}

esp_err_t Display::ShowFull(const Canvas& canvas) {
    if (epd_ == nullptr) return ESP_ERR_INVALID_STATE;

    const int64_t started = esp_timer_get_time();
    const esp_err_t err =
        zectrix_epd_refresh_full_1bpp(epd_, canvas.data(), Canvas::size());
    ESP_LOGI(kTag, "full refresh %lldms", (esp_timer_get_time() - started) / 1000);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "full refresh failed: %s", esp_err_to_name(err));
        shown_valid_ = false;
        return err;
    }

    std::memcpy(shown_.data(), canvas.data(), Canvas::size());
    shown_valid_ = true;
    partials_since_full_ = 0;
    return ESP_OK;
}

int Display::FindDirtyBands(const Canvas& canvas, Band* bands, int max_bands) {
    const uint8_t* fresh = canvas.data();
    const uint8_t* old = shown_.data();

    int count = 0;
    Band current = {};
    bool open = false;
    int clean_run = 0;

    for (int row = 0; row < Canvas::kHeight; ++row) {
        const int base = row * Canvas::kStride;
        const bool dirty =
            std::memcmp(fresh + base, old + base, Canvas::kStride) != 0;

        if (!dirty) {
            if (open && ++clean_run > kBandGap) {
                // Out of slots: fold this band into the previous one rather
                // than giving up. An over-tall rectangle costs a few
                // milliseconds; bailing out costs a full-refresh flash.
                if (count >= max_bands) {
                    bands[count - 1].bottom = current.bottom;
                    if (current.min_byte < bands[count - 1].min_byte) {
                        bands[count - 1].min_byte = current.min_byte;
                    }
                    if (current.max_byte > bands[count - 1].max_byte) {
                        bands[count - 1].max_byte = current.max_byte;
                    }
                } else {
                    bands[count++] = current;
                }
                open = false;
            }
            continue;
        }

        // Byte-granular horizontal bounds. Rounding out to byte boundaries
        // costs at most 7 pixels a side and saves unpacking every bit.
        int min_byte = Canvas::kStride;
        int max_byte = 0;
        for (int b = 0; b < Canvas::kStride; ++b) {
            if (fresh[base + b] == old[base + b]) continue;
            if (b < min_byte) min_byte = b;
            if (b > max_byte) max_byte = b;
        }

        if (!open) {
            current.top = row;
            current.min_byte = min_byte;
            current.max_byte = max_byte;
            open = true;
        } else {
            if (min_byte < current.min_byte) current.min_byte = min_byte;
            if (max_byte > current.max_byte) current.max_byte = max_byte;
        }
        current.bottom = row;
        clean_run = 0;
    }

    if (open) {
        if (count >= max_bands) {
            bands[count - 1].bottom = current.bottom;
            if (current.min_byte < bands[count - 1].min_byte) {
                bands[count - 1].min_byte = current.min_byte;
            }
            if (current.max_byte > bands[count - 1].max_byte) {
                bands[count - 1].max_byte = current.max_byte;
            }
        } else {
            bands[count++] = current;
        }
    }
    return count;
}

// Collapses every band into the single rectangle that encloses them all.
// One tall refresh beats several short ones, and beats a full-refresh flash.
void Display::CollapseBands(Band* bands, int* count) {
    for (int i = 1; i < *count; ++i) {
        bands[0].bottom = bands[i].bottom;
        if (bands[i].min_byte < bands[0].min_byte) {
            bands[0].min_byte = bands[i].min_byte;
        }
        if (bands[i].max_byte > bands[0].max_byte) {
            bands[0].max_byte = bands[i].max_byte;
        }
    }
    *count = 1;
}

esp_err_t Display::Show(const Canvas& canvas, bool defer_full) {
    if (epd_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!shown_valid_) {
        ESP_LOGW(kTag, "full: no valid baseline");
        return ShowFull(canvas);
    }

    // Splitting the diff into bands keeps an unrelated change at the other end
    // of the panel from dragging the whole screen into one huge rectangle.
    Band bands[kMaxBands];
    int count = FindDirtyBands(canvas, bands, kMaxBands);

    if (count == 0) return ESP_OK;  // nothing moved

    // Refresh cost barely varies with area -- measured, a 20-row partial is
    // 776ms and a whole-screen one 917ms, against 1219ms for a full refresh.
    // So a single partial always beats a full frame however much it covers,
    // and what matters is only how many refreshes a frame needs.
    const bool too_many = count * kPartialCostMs >= kFullCostMs;
    const bool over_budget = partials_since_full_ + count > kMaxPartials;

    if (defer_full) {
        // A running session never flashes. If the frame is scattered enough
        // that several partials would cost more than a full refresh, redraw
        // one rectangle covering the lot instead: worst case that is the whole
        // panel at 917ms, still a quiet update rather than a black flash.
        if (too_many) CollapseBands(bands, &count);
    } else if (too_many || over_budget) {
        return ShowFull(canvas);
    }

    const int64_t started = esp_timer_get_time();
    for (int i = 0; i < count; ++i) {
        const zectrix_epd_rect_t rect = {
            bands[i].min_byte * 8,
            bands[i].top,
            (bands[i].max_byte - bands[i].min_byte + 1) * 8,
            bands[i].bottom - bands[i].top + 1,
        };
        canvas.ExtractRect(rect.x, rect.y, rect.width, rect.height, patch_);
        const size_t patch_size =
            static_cast<size_t>((rect.width + 7) / 8) * rect.height;

        const esp_err_t err =
            zectrix_epd_refresh_partial_1bpp(epd_, &rect, patch_, patch_size);
        if (err != ESP_OK) {
            // The driver drops its own baseline on failure, so recover with a
            // clean frame rather than stacking another partial on top.
            ESP_LOGW(kTag, "partial refresh failed: %s", esp_err_to_name(err));
            shown_valid_ = false;
            return ShowFull(canvas);
        }
        ++partials_since_full_;
    }

    ESP_LOGI(kTag, "%d partial(s) %lldms", count,
             (esp_timer_get_time() - started) / 1000);

    std::memcpy(shown_.data(), canvas.data(), Canvas::size());
    return ESP_OK;
}
