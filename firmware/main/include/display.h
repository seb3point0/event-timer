#ifndef ZT_DISPLAY_H_
#define ZT_DISPLAY_H_

#include "canvas.h"
#include "esp_err.h"
#include "zectrix_epd.h"

// Chooses between partial and full refresh by diffing the new frame against
// what is physically on the panel.
//
// Doing it here rather than at each call site means the countdown pays for a
// small rectangle of digits, screen changes pay for a clean full refresh, and
// no caller has to remember which is which.
class Display {
public:
    esp_err_t Begin(zectrix_epd_handle_t epd);

    // Pushes the frame, picking the cheapest refresh that still looks right.
    // `defer_full` holds off the periodic ghost-clearing full refresh, for the
    // stretch of a talk where a 1.2s flash across the panel would land in front
    // of a speaker watching their last seconds tick away.
    esp_err_t Show(const Canvas& canvas, bool defer_full = false);
    // Forces a full refresh, which also clears accumulated ghosting.
    esp_err_t ShowFull(const Canvas& canvas);

private:
    // A contiguous run of changed rows, with the byte-aligned horizontal
    // bounds of everything that changed inside it.
    struct Band {
        int top;
        int bottom;
        int min_byte;
        int max_byte;
    };

    // Partial refreshes leave a faint residue behind, so periodically one gets
    // promoted to a full refresh to wipe it out. At one update a second this
    // has to be large, because that promotion is a 1.2s black flash across the
    // panel and there is no good moment for one during a talk. Phase changes
    // do a full refresh anyway, which is the real ghosting reset.
    static constexpr int kMaxPartials = 400;
    // Bands closer together than this are merged: two refreshes with a sliver
    // of clean rows between them cost far more than one slightly taller
    // refresh. Generous on purpose. The clock and the progress bar under it
    // are ~12 rows apart, but digit glyphs differ in height by a few pixels
    // ('7' is 127 rows, '0' is 131), so the gap breathes as the numbers
    // change. Too tight a threshold and the two merge on some seconds and
    // split on others, which is exactly how an intermittent flash gets in.
    static constexpr int kBandGap = 28;
    static constexpr int kMaxBands = 4;

    // Measured on the fitted panel. A refresh is dominated by the waveform
    // sequence rather than by how many pixels move, which is why two partials
    // are worse than one full frame.
    static constexpr int kPartialCostMs = 808;
    static constexpr int kFullCostMs = 1247;

    // Returns the number of bands found, always between 0 and max_bands: a
    // frame with more separate regions than that gets the extras folded into
    // the last band rather than failing.
    int FindDirtyBands(const Canvas& canvas, Band* bands, int max_bands);
    static void CollapseBands(Band* bands, int* count);

    zectrix_epd_handle_t epd_ = nullptr;
    Canvas shown_;
    bool shown_valid_ = false;
    int partials_since_full_ = 0;
    uint8_t patch_[ZECTRIX_EPD_1BPP_FRAME_BYTES];
};

#endif  // ZT_DISPLAY_H_
