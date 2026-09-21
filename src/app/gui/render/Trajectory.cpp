// Trajectory.cpp -- see Trajectory.h.

#include "app/gui/render/Trajectory.h"

#include "core/Similarity.h"

#include <algorithm>
#include <cmath>

namespace gui::render {

namespace {

// The segment holding `t` and how far along it, 0..1.
int segment_of(const std::vector<Keyframe>& keys, double t, double& u) {
    const int n = (int)keys.size();
    if (t <= keys[0].time) { u = 0.0; return 0; }
    if (t >= keys[(size_t)n - 1].time) { u = 1.0; return std::max(0, n - 2); }
    int i = 0;
    while (i + 2 < n && t >= keys[(size_t)i + 1].time) i++;
    const double dt = keys[(size_t)i + 1].time - keys[(size_t)i].time;
    u = dt > 1e-12 ? (t - keys[(size_t)i].time) / dt : 1.0;
    return i;
}

// Cubic Hermite from p0 to p1 with end slopes m0, m1 (per unit of u).
double hermite(double p0, double p1, double m0, double m1, double u) {
    const double u2 = u * u, u3 = u2 * u;
    return (2*u3 - 3*u2 + 1) * p0 + (u3 - 2*u2 + u) * m0 +
           (-2*u3 + 3*u2) * p1 + (u3 - u2) * m1;
}

double smoothstep(double u) { return u * u * (3.0 - 2.0 * u); }

void normalize4(double q[4]) {
    const double n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-300) for (int k = 0; k < 4; k++) q[k] /= n;
    else { q[0] = 1; q[1] = q[2] = q[3] = 0; }
}

void slerp(const double a[4], const double b[4], double u, double out[4]) {
    double d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const double sgn = d < 0 ? -1.0 : 1.0;
    d = std::fabs(d);
    double ka = 1.0 - u, kb = u;
    if (d < 0.9999) {
        const double th = std::acos(d), s = std::sin(th);
        ka = std::sin((1.0 - u) * th) / s;
        kb = std::sin(u * th) / s;
    }
    for (int k = 0; k < 4; k++) out[k] = ka * a[k] + kb * sgn * b[k];
    normalize4(out);
}

}  // namespace


void quat_mul(const double a[4], const double b[4], double o[4]) {
    const double r[4] = {a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3],
                         a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2],
                         a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1],
                         a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0]};
    for (int k = 0; k < 4; k++) o[k] = r[k];
}

void quat_to_matrix3(const double q[4], double R[9]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0] = 1 - 2*(y*y + z*z); R[1] = 2*(x*y - w*z);     R[2] = 2*(x*z + w*y);
    R[3] = 2*(x*y + w*z);     R[4] = 1 - 2*(x*x + z*z); R[5] = 2*(y*z - w*x);
    R[6] = 2*(x*z - w*y);     R[7] = 2*(y*z + w*x);     R[8] = 1 - 2*(x*x + y*y);
}

void quat_from_matrix3(const double R[9], double q[4]) {
    spirula::Sim3 s;
    for (int i = 0; i < 9; i++) s.R[i] = R[i];
    s.orthonormalize();
    s.quat(q);
}

void quat_rotate(const double q[4], const double v[3], double out[3]) {
    double R[9];
    quat_to_matrix3(q, R);
    for (int r = 0; r < 3; r++)
        out[r] = R[r*3+0]*v[0] + R[r*3+1]*v[1] + R[r*3+2]*v[2];
}

void CameraState::c2w(double out[12]) const {
    double R[9];
    quat_to_matrix3(rot, R);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r*4+c] = R[r*3+c];
        out[r*4+3] = pos[r];
    }
}


// ===========================================================================
// Trajectory
// ===========================================================================

Trajectory::Trajectory(const RenderProject& p) : _p(p) {
    const std::vector<Keyframe>& k = p.keys;
    const int n = (int)k.size();
    if (n == 0) return;

    // Rotations on one side of the double cover, so the spline through them
    // turns the short way at every key.
    _rot.resize((size_t)n * 4);
    for (int i = 0; i < n; i++) {
        double q[4] = {k[(size_t)i].rot[0], k[(size_t)i].rot[1],
                       k[(size_t)i].rot[2], k[(size_t)i].rot[3]};
        if (i > 0) {
            const double* prev = &_rot[(size_t)(i - 1) * 4];
            if (q[0]*prev[0] + q[1]*prev[1] + q[2]*prev[2] + q[3]*prev[3] < 0)
                for (double& v : q) v = -v;
        }
        for (int c = 0; c < 4; c++) _rot[(size_t)i * 4 + c] = q[c];
    }

    // Tangents in value per second: the time-weighted Catmull-Rom slope, less
    // the tension, zero at a hold and an eased end. Places are Fritsch-Carlson
    // limited, or unevenly timed keys send the camera past a key and back.
    auto tangents = [&](int dims, auto value, std::vector<double>& out,
                        auto usable, bool monotone) {
        out.assign((size_t)n * dims, 0.0);
        if (n < 2 || !_p.motion.smooth) return;
        for (int i = 0; i < n; i++) {
            const Keyframe& ki = k[(size_t)i];
            if (ki.hold || !usable(i)) continue;
            const bool first = i == 0, last = i == n - 1;
            if ((first || last) && _p.motion.ease) continue;
            for (int d = 0; d < dims; d++) {
                double m;
                if (first || last || !usable(i - 1) || !usable(i + 1)) {
                    const int a = first ? 0 : i - 1, b = last ? n - 1 : i + 1;
                    const int lo = (first || !usable(i - 1)) ? i : a;
                    const int hi = (last || !usable(i + 1)) ? i : b;
                    const double dt = k[(size_t)hi].time - k[(size_t)lo].time;
                    m = dt > 1e-9 ? (value(hi, d) - value(lo, d)) / dt : 0.0;
                } else {
                    const double t0 = k[(size_t)i - 1].time, t1 = ki.time,
                                 t2 = k[(size_t)i + 1].time;
                    const double d0 = t1 - t0, d1 = t2 - t1;
                    if (d0 <= 1e-9 || d1 <= 1e-9) { m = 0.0; }
                    else {
                        const double s0 = (value(i, d) - value(i - 1, d)) / d0;
                        const double s1 = (value(i + 1, d) - value(i, d)) / d1;
                        m = (s0 * d1 + s1 * d0) / (d0 + d1);
                    }
                }
                out[(size_t)i * dims + d] = (1.0 - _p.motion.tension) * m;
            }
            // On the whole vector, so a turned path moves as a turned path: no
            // faster than 3x the slower chord, easing off smoothly towards rest
            // as the path doubles back (a threshold would make it a knife edge).
            if (!monotone || i == 0 || i == n - 1 || !usable(i - 1) || !usable(i + 1)) continue;
            const double d0 = ki.time - k[(size_t)i - 1].time, d1 = k[(size_t)i + 1].time - ki.time;
            if (d0 <= 1e-9 || d1 <= 1e-9) continue;
            double v0 = 0.0, v1 = 0.0, dot = 0.0, mm = 0.0;
            for (int d = 0; d < dims; d++) {
                const double a = (value(i, d) - value(i - 1, d)) / d0;
                const double b = (value(i + 1, d) - value(i, d)) / d1;
                v0 += a * a;
                v1 += b * b;
                dot += a * b;
                mm += out[(size_t)i * dims + d] * out[(size_t)i * dims + d];
            }
            const double cap = 3.0 * std::sqrt(std::min(v0, v1));
            const double cosine = v0 > 0.0 && v1 > 0.0 ? dot / std::sqrt(v0 * v1) : 1.0;
            const double scale = std::clamp(1.0 + cosine, 0.0, 1.0) *
                                 (mm > cap * cap ? cap / std::sqrt(mm) : 1.0);
            for (int d = 0; d < dims; d++) out[(size_t)i * dims + d] *= scale;
        }
    };
    auto always = [](int) { return true; };
    tangents(3, [&](int i, int d) { return k[(size_t)i].pos[d]; }, _dpos, always, true);
    tangents(4, [&](int i, int d) { return _rot[(size_t)i * 4 + d]; }, _drot, always, false);
    auto aimed = [&](int i) { return k[(size_t)i].aim; };
    tangents(3, [&](int i, int d) { return k[(size_t)i].target[d]; }, _dtgt, aimed, true);
    tangents(1, [&](int i, int) { return k[(size_t)i].roll; }, _droll, aimed, false);
    tangents(1, [&](int i, int) { return std::log(std::max(_p.lens_at(i).focal, 1e-6)); },
             _dfocal, always, true);

    // Arc length against key time, for constant speed and for the readout.
    const int per = 48;
    _cum_time.push_back(k[0].time);
    _cum_len.push_back(0.0);
    const double t0 = k[0].time, t1 = k[(size_t)n - 1].time;
    const int steps = std::max(1, (n - 1) * per);
    double last[3] = {k[0].pos[0], k[0].pos[1], k[0].pos[2]};
    for (int s = 1; s <= steps && t1 > t0; s++) {
        const double t = t0 + (t1 - t0) * s / steps;
        const CameraState c = eval(t);
        const double dx = c.pos[0] - last[0], dy = c.pos[1] - last[1],
                     dz = c.pos[2] - last[2];
        _length += std::sqrt(dx*dx + dy*dy + dz*dz);
        for (int d = 0; d < 3; d++) last[d] = c.pos[d];
        _cum_time.push_back(t);
        _cum_len.push_back(_length);
    }
}

double Trajectory::warp(double t) const {
    const std::vector<Keyframe>& k = _p.keys;
    if (k.size() < 2 || !_p.motion.constant_speed || !(_length > 1e-12))
        return t;
    const double t0 = k.front().time, t1 = k.back().time;
    if (t <= t0 || t1 <= t0) return t0;
    if (t >= t1) return t1;
    double u = (t - t0) / (t1 - t0);
    if (_p.motion.ease) u = smoothstep(u);
    const double s = u * _length;
    const auto it = std::lower_bound(_cum_len.begin(), _cum_len.end(), s);
    if (it == _cum_len.begin()) return _cum_time.front();
    if (it == _cum_len.end()) return _cum_time.back();
    const size_t j = (size_t)(it - _cum_len.begin());
    const double l0 = _cum_len[j - 1], l1 = _cum_len[j];
    const double f = l1 > l0 ? (s - l0) / (l1 - l0) : 0.0;
    return _cum_time[j - 1] + f * (_cum_time[j] - _cum_time[j - 1]);
}

CameraState Trajectory::at(double t) const { return eval(warp(t)); }

CameraState Trajectory::eval(double t) const {
    CameraState c;
    const std::vector<Keyframe>& k = _p.keys;
    if (k.empty()) return c;
    if (k.size() == 1) {
        for (int d = 0; d < 3; d++) c.pos[d] = k[0].pos[d];
        for (int d = 0; d < 4; d++) c.rot[d] = k[0].rot[d];
        c.lens = _p.lens_at(0);
        return c;
    }
    double u;
    const int i = segment_of(k, t, u);
    const Keyframe& a = k[(size_t)i];
    const Keyframe& b = k[(size_t)i + 1];
    const double dt = b.time - a.time;
    // An end that eases in straight lines still has to start from rest.
    double v = u;
    if (!_p.motion.smooth && _p.motion.ease && !_p.motion.constant_speed &&
        (i == 0 || i + 2 == (int)k.size())) {
        const bool first = i == 0, last = i + 2 == (int)k.size();
        if (first && last) v = smoothstep(u);
        else if (first) v = u * u * (2.0 - u);   // rest at 0, full speed at 1
        else if (last) v = u * (1.0 + u - u * u);
    }
    auto channel = [&](double p0, double p1, const std::vector<double>& m,
                       int dims, int d) {
        if (!_p.motion.smooth) return p0 + (p1 - p0) * v;
        return hermite(p0, p1, m[(size_t)i * dims + d] * dt,
                       m[(size_t)(i + 1) * dims + d] * dt, u);
    };

    for (int d = 0; d < 3; d++) c.pos[d] = channel(a.pos[d], b.pos[d], _dpos, 3, d);

    if (a.aim && b.aim) {
        double tgt[3];
        for (int d = 0; d < 3; d++) tgt[d] = channel(a.target[d], b.target[d], _dtgt, 3, d);
        const double roll = channel(a.roll, b.roll, _droll, 1, 0);
        aim_rotation(c.pos, tgt, _p.up, roll, c.rot);
    } else if (_p.motion.smooth) {
        const double* qa = &_rot[(size_t)i * 4];
        const double* qb = &_rot[(size_t)(i + 1) * 4];
        for (int d = 0; d < 4; d++) c.rot[d] = channel(qa[d], qb[d], _drot, 4, d);
        normalize4(c.rot);
    } else {
        slerp(&_rot[(size_t)i * 4], &_rot[(size_t)(i + 1) * 4], v, c.rot);
    }

    // The projection and the distortion model cannot be blended, so they
    // change at the key; the zoom and the coefficients glide.
    const Lens& la = _p.lens_at(i);
    const Lens& lb = _p.lens_at(i + 1);
    c.lens = u >= 1.0 ? lb : la;
    if (la.projection == lb.projection) {
        const double f = channel(std::log(std::max(la.focal, 1e-6)),
                                 std::log(std::max(lb.focal, 1e-6)), _dfocal, 1, 0);
        c.lens.focal = std::exp(f);
        if (la.tier == lb.tier)
            for (int d = 0; d < kLensCoeffs; d++)
                c.lens.dist[d] = (float)(la.dist[d] + (lb.dist[d] - la.dist[d]) * smoothstep(u));
    }
    return c;
}

std::vector<double> Trajectory::sample_path(int n) const {
    std::vector<double> out;
    const std::vector<Keyframe>& k = _p.keys;
    if (k.empty() || n < 2) return out;
    const double t0 = k.front().time, t1 = k.back().time;
    out.reserve((size_t)n * 3);
    for (int s = 0; s < n; s++) {
        const CameraState c = eval(t0 + (t1 - t0) * s / (n - 1));
        out.insert(out.end(), c.pos, c.pos + 3);
    }
    return out;
}

}  // namespace gui::render
