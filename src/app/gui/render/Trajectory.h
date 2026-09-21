#pragma once

// Where the camera is at a given time: the keyframes of a RenderProject
// joined into one motion. Every channel -- position, rotation, aim, zoom --
// is a cubic through the keys on one shared parameter: key time, or the
// distance along the path when the speed is held constant. The Spline curve
// is C2 (a cubic B-spline through the keys), Catmull-Rom is C1 and local.

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
    // The path as `n` evenly spaced positions, for drawing it.
    std::vector<double> sample_path(int n) const;
    double length() const { return _length; }

private:
    // Curve parameter of the frame at `t`.
    double warp(double t) const;
    CameraState eval(double u) const;

    const RenderProject& _p;
    // Knots: one per key, and a loop repeats the first at its end.
    int _n = 0, _knots = 0;
    bool _closed = false;
    std::vector<double> _u;              // parameter at each knot
    std::vector<int> _src;               // the key each knot is
    std::vector<double> _t;              // output time at each knot
    // Per knot: every channel's value and slope (per unit of parameter).
    std::vector<double> _pos, _rot, _tgt, _roll, _logf;
    std::vector<double> _dpos, _drot, _dtgt, _droll, _dfocal;
    std::vector<double> _cum_u, _cum_len;    // arc-length table
    double _length = 0.0;
};

// The quaternion of `a` then `b`, and a rotation of a vector, (w, x, y, z).
void quat_mul(const double a[4], const double b[4], double out[4]);
void quat_rotate(const double q[4], const double v[3], double out[3]);
void quat_to_matrix3(const double q[4], double R[9]);
void quat_from_matrix3(const double R[9], double q[4]);

}  // namespace gui::render
