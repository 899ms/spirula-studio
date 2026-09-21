#pragma once

// Where the camera is at a given time: the keyframes of a RenderProject
// joined into one motion. Position, rotation, aim and zoom all run on the
// same time-based Hermite spline, so a hold or an eased end stops all of
// them together; constant speed is a warp of time along the path's length.

#include "app/gui/render/RenderProject.h"

#include <vector>

namespace gui::render {

// What one frame is rendered from.
struct CameraState {
    double pos[3] = {0, 0, 0};
    double rot[4] = {1, 0, 0, 0};       // camera-to-world, (w, x, y, z)
    Lens lens;
    // Row-major 3x4 camera-to-world, OpenGL axes: what the renderers take.
    void c2w(double out[12]) const;
};

// Precomputed once per edit of the project, then evaluated per frame.
class Trajectory {
public:
    explicit Trajectory(const RenderProject& p);
    CameraState at(double t) const;
    // The path as `n` evenly timed positions, for drawing it.
    std::vector<double> sample_path(int n) const;
    double length() const { return _length; }

private:
    // Keyframe time a frame at `t` is evaluated at: `t` itself, or where the
    // arc length says it is when the speed is held constant.
    double warp(double t) const;
    CameraState eval(double t) const;

    const RenderProject& _p;
    // Per key: the tangents of every channel, in units per second.
    std::vector<double> _dpos, _drot, _dtgt, _droll, _dfocal;
    std::vector<double> _rot;            // keys' rotations, hemisphere-aligned
    std::vector<double> _cum_time, _cum_len;   // arc-length table
    double _length = 0.0;
};

// The quaternion of `a` then `b`, and a rotation of a vector, (w, x, y, z).
void quat_mul(const double a[4], const double b[4], double out[4]);
void quat_rotate(const double q[4], const double v[3], double out[3]);
void quat_to_matrix3(const double q[4], double R[9]);
void quat_from_matrix3(const double R[9], double q[4]);

}  // namespace gui::render
