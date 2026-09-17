#pragma once

// FrameMotion -- how fast a video's view is changing, and the frame spacing
// that follows from it.
//
// A grid of points tracked between small grey frames, one global model fitted
// to them -- a 2D affine, or a rotation of the sphere where the capture sees
// all of it -- and two numbers out: how much of the view left it, and how much
// of the flow that model could not explain. A capture that only turns on the
// spot scores nothing on the second, which is what tells a pan apart from a
// walk past something close. docs/datasets.md weighs them.

#include "app/Pano360.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace app {

// What the grey frames are pictures of, which is what decides whether turning
// the camera costs anything: a full-sphere capture keeps every direction it
// had, a rectilinear one does not.
enum class MotionView { Planar, Packed360, Fisheye };

struct MotionOptions {
    MotionView view = MotionView::Planar;
    int width = 0, height = 0;      // of the grey frames handed to track()
    // Packed360: the packing, in SOURCE pixels, of the ONE track the grey
    // frames come from -- half the sphere is enough to fit a rotation to.
    Pano360Layout eac;
    // Fisheye: the image circle in grey-frame pixels (0 = the inscribed one).
    // Every 360 lens is a little over 180 and none of them agree; a few degrees
    // of error leaves less residual than the tracking floor already does.
    float circle_x = 0, circle_y = 0, circle_r = 0;
    float circle_fov = 3.4034f;      // full angle across the circle, radians
    // Radians across the diagonal of what the dataset will hold, which is what
    // an angular residual is a fraction of.
    float out_fov = 1.5708f;
};

// Tracks one stream of grey frames. Costs are readable only after the last
// track(): a fisheye's field of view is fitted from the first few steps and
// their costs are revised once it is.
class MotionTracker {
public:
    explicit MotionTracker(const MotionOptions& options);
    ~MotionTracker();
    MotionTracker(const MotionTracker&) = delete;
    MotionTracker& operator=(const MotionTracker&) = delete;

    // `gray` is width*height bytes; `index` is the source frame it came from.
    // The first call only primes the reference.
    void track(const uint8_t* gray, int64_t index);

    // Fills in the steps nothing could be tracked across. Call it once, after
    // the last track().
    void finish();

    // One entry per step, in order: the view change across it, and the source
    // frame it ends at. Both are final only after finish().
    const std::vector<float>& costs() const;
    const std::vector<int64_t>& ends() const;
    // Steps whose flow was too weak to fit a model to, for the log.
    int weak_steps() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The angle across the diagonal of the views a 360 plan writes, which is what
// an angular residual is a fraction of. 90 degrees without a plan.
float motion_out_fov(const std::vector<Pano360View>& views);

// The grey frames to hand the tracker, for a source of this size. A full
// sphere spans three times the angle a rectilinear frame does, so it is given
// the pixels to match or its own tracking noise drowns the parallax.
void motion_frame_size(MotionView view, int src_w, int src_h, int& w, int& h);

// One video's measured view change: what MotionTracker produced, and what the
// fixed schedule would have done with it.
struct MotionPlanInput {
    std::vector<float> cost;
    std::vector<int64_t> ends;
    int64_t frames = 0;
    int skip = 1;        // the fixed schedule's spacing
    int window = 1;      // the sharpness window, the closest two frames may be
    int max_frames = 0;  // 0 = no cap
    double fps = 0;      // the source's own rate; only for reporting
};

// Which source frames each run should end its sharpness windows at, so that
// kept frames differ by view rather than by time, within `range` of the rate
// `skip` asks for. Several share ONE budget: what moves more gets more of it.
std::vector<std::vector<int64_t>> plan_by_motion(
    const std::vector<MotionPlanInput>& in, float range);

std::vector<int64_t> plan_by_motion(const std::vector<float>& cost,
                                    const std::vector<int64_t>& ends,
                                    int64_t frames, int skip, int window,
                                    float range, int max_frames);

}  // namespace app
