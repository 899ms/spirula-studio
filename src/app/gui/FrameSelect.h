#pragma once

// FrameSelect -- which of the candidate frames ffmpeg extracted are kept.
//
// The sharpness metric is the variance of the 3x3 Laplacian of the
// mean-subtracted 512x512 grayscale image. Evenly, one per group of `group`
// candidates; or spaced by how much the view changes (app/FrameMotion.h),
// which is what the built-in decoder's adaptive mode does from the source
// frames themselves.

#include "app/FrameMotion.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace gui {

struct FrameSelectOptions {
    int group = 1;          // candidates per kept frame
    int max_frames = 0;     // 0 = no cap
    // Adaptive: the rate stays within `range` either side of the average, and
    // the sharpest of `window` candidates around each chosen one is kept.
    bool  adaptive = false;
    float range = 4.0f;
    int   window = 3;
    app::MotionView view = app::MotionView::Planar;
    // Set when the candidates are 360 canvases. An EAC one is measured on its
    // top row alone, which is half the sphere and all a rotation needs.
    app::Pano360Layout eac;
    float out_fov = 1.5708f;
    // (candidates scored, candidates in all), and the spacing settled on: the
    // candidate each kept frame ends at, out of that many. Both optional.
    std::function<void(int64_t, int64_t)> scanning;
    std::function<void(const std::vector<int64_t>&, int64_t)> planned;
    // Each candidate's view change as it is measured, for a live curve:
    // the candidate it ends at, how many there are, and the change.
    std::function<void(int64_t, int64_t, float)> measured;
};

// Scores every image in cand_dir (sorted by filename) across all hardware
// threads and MOVES the keepers to out_dir as <prefix>NNNNN.<ext>, numbered by
// their place in cand_dir; the losers are deleted. -1 on error or cancellation.
int select_sharpest_frames(const std::string& cand_dir,
                           const std::string& out_dir,
                           const std::string& prefix,
                           const FrameSelectOptions& options,
                           const std::function<void(const std::string&)>& log,
                           const std::atomic<bool>& cancel);

}  // namespace gui
