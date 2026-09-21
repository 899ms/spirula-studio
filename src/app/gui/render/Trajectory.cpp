// Trajectory.cpp -- see Trajectory.h.

#include "app/gui/render/Trajectory.h"

#include "core/Similarity.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gui::render {

namespace {

// The segment of `knots` holding `x` and how far along it, 0..1.
int segment_of(const std::vector<double>& knots, double x, double& f) {
    const int n = (int)knots.size();
    if (x <= knots[0]) { f = 0.0; return 0; }
    if (x >= knots[(size_t)n - 1]) { f = 1.0; return std::max(0, n - 2); }
    const int i = std::max(0, (int)(std::upper_bound(knots.begin(), knots.end(), x) -
                                    knots.begin()) - 1);
    const int j = std::min(i, n - 2);
    const double d = knots[(size_t)j + 1] - knots[(size_t)j];
    f = d > 1e-12 ? std::clamp((x - knots[(size_t)j]) / d, 0.0, 1.0) : 1.0;
    return j;
}

// Tridiagonal solve in place: `a` below the diagonal, `b` on it, `c` above;
// the answer replaces `d`.
void thomas(std::vector<double> a, std::vector<double> b, const std::vector<double>& c,
            std::vector<double>& d) {
    const size_t m = b.size();
    for (size_t i = 1; i < m; i++) {
        const double w = a[i] / b[i - 1];
        b[i] -= w * c[i - 1];
        d[i] -= w * d[i - 1];
    }
    d[m - 1] /= b[m - 1];
    for (size_t i = m - 1; i-- > 0;) d[i] = (d[i] - c[i] * d[i + 1]) / b[i];
}

// The same with the two corners filled -- `beta` top right, `alpha` bottom
// left -- by Sherman-Morrison; m >= 3.
void cyclic(const std::vector<double>& a, std::vector<double> b, const std::vector<double>& c,
            double alpha, double beta, std::vector<double>& d) {
    const size_t m = b.size();
    const double gamma = -b[0];
    b[0] -= gamma;
    b[m - 1] -= alpha * beta / gamma;
    std::vector<double> z(m, 0.0);
    z[0] = gamma;
    z[m - 1] = alpha;
    thomas(a, b, c, d);
    thomas(a, b, c, z);
    const double fact = (d[0] + beta * d[m - 1] / gamma) / (1.0 + z[0] + beta * z[m - 1] / gamma);
    for (size_t i = 0; i < m; i++) d[i] -= fact * z[i];
}

// A run of knots one channel solves together, as positions along an
// unrolled loop: the knot, its parameter, and the sign a rotation takes
// past the loop's end (a full turn comes back as -q).
struct Elem { int knot; double u; double sign; };

// C2 slopes through `e` for one value per knot, ends held at rest or left
// free (zero second derivative).
void spline_piece(const std::vector<Elem>& e, const std::vector<double>& y,
                  bool rest_a, bool rest_b, std::vector<double>& m) {
    const size_t k = e.size();
    std::vector<double> a(k, 0.0), b(k, 0.0), c(k, 0.0), d(k, 0.0);
    auto h = [&](size_t j) { return e[j + 1].u - e[j].u; };
    auto delta = [&](size_t j) { return (y[j + 1] - y[j]) / h(j); };
    for (size_t j = 0; j < k; j++) {
        if ((j == 0 && rest_a) || (j == k - 1 && rest_b)) { b[j] = 1.0; continue; }
        if (j == 0) { b[j] = 2.0 / h(0); c[j] = 1.0 / h(0); d[j] = 3.0 * delta(0) / h(0); continue; }
        if (j == k - 1) {
            a[j] = 1.0 / h(j - 1); b[j] = 2.0 / h(j - 1); d[j] = 3.0 * delta(j - 1) / h(j - 1);
            continue;
        }
        a[j] = 1.0 / h(j - 1);
        c[j] = 1.0 / h(j);
        b[j] = 2.0 * (a[j] + c[j]);
        d[j] = 3.0 * (delta(j - 1) / h(j - 1) + delta(j) / h(j));
    }
    thomas(a, b, c, d);
    m = d;
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
    _n = (int)k.size();
    if (_n == 0) return;
    _closed = p.looped();
    _knots = _closed ? _n + 1 : _n;
    const int K = _knots, n = _n;
    _src.resize((size_t)K);
    _t.resize((size_t)K);
    for (int i = 0; i < K; i++) {
        _src[(size_t)i] = i < n ? i : 0;
        _t[(size_t)i] = i < n ? k[(size_t)i].time : p.duration();
    }
    auto key = [&](int knot) -> const Keyframe& { return k[(size_t)_src[(size_t)knot]]; };

    // Values at every knot. Rotations on one side of the double cover, so
    // the curve through them turns the short way at every key.
    _pos.resize((size_t)K * 3);
    _tgt.resize((size_t)K * 3);
    _rot.resize((size_t)K * 4);
    _roll.resize((size_t)K);
    _logf.resize((size_t)K);
    for (int i = 0; i < K; i++) {
        const Keyframe& ki = key(i);
        for (int d = 0; d < 3; d++) {
            _pos[(size_t)i * 3 + d] = ki.pos[d];
            _tgt[(size_t)i * 3 + d] = ki.target[d];
        }
        double q[4] = {ki.rot[0], ki.rot[1], ki.rot[2], ki.rot[3]};
        if (i > 0) {
            const double* prev = &_rot[(size_t)(i - 1) * 4];
            if (q[0]*prev[0] + q[1]*prev[1] + q[2]*prev[2] + q[3]*prev[3] < 0)
                for (double& v : q) v = -v;
        }
        for (int c = 0; c < 4; c++) _rot[(size_t)i * 4 + c] = q[c];
        _roll[(size_t)i] = ki.roll;
        _logf[(size_t)i] = std::log(std::max(p.lens_at(_src[(size_t)i]).focal, 1e-6));
    }
    const double rot_sign = _closed && (_rot[(size_t)n * 4] * _rot[0] + _rot[(size_t)n * 4 + 1] * _rot[1] +
                                        _rot[(size_t)n * 4 + 2] * _rot[2] +
                                        _rot[(size_t)n * 4 + 3] * _rot[3]) < 0 ? -1.0 : 1.0;

    // The parameter: key time, or with the speed held constant the distance
    // along the keys, so unevenly timed keys cannot make the curve overshoot.
    _u = _t;
    if (p.motion.constant_speed && K >= 2) {
        std::vector<double> chord((size_t)K - 1);
        double total = 0.0;
        for (int i = 0; i + 1 < K; i++) {
            const double* a = &_pos[(size_t)i * 3];
            const double* b = &_pos[(size_t)(i + 1) * 3];
            chord[(size_t)i] = std::sqrt((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) +
                                         (b[2]-a[2])*(b[2]-a[2]));
            total += chord[(size_t)i];
        }
        if (total > 1e-12) {
            const double floor = 1e-4 * total / (K - 1);
            _u[0] = 0.0;
            for (int i = 0; i + 1 < K; i++)
                _u[(size_t)i + 1] = _u[(size_t)i] + std::max(chord[(size_t)i], floor);
        }
    }
    const double period = _closed ? _u[(size_t)n] - _u[0] : 0.0;

    // A key the camera stops at: a hold, and an eased end of an open move.
    std::vector<uint8_t> rest((size_t)K, 0);
    for (int i = 0; i < K; i++) rest[(size_t)i] = key(i).hold ? 1 : 0;
    if (!_closed && p.motion.ease) rest[0] = rest[(size_t)K - 1] = 1;

    // Slopes for one channel. `joined(i)`: the interval from knot i to i + 1
    // is one smooth piece for this channel.
    const Curve curve = p.motion.curve;
    auto solve = [&](int dims, const std::vector<double>& val, std::vector<double>& out,
                     auto joined_raw, double sign, bool monotone) {
        out.assign((size_t)K * dims, 0.0);
        if (K < 2 || curve == Curve::Linear) return;
        auto joined = [&](int i) {
            return _u[(size_t)i + 1] - _u[(size_t)i] > 1e-9 && joined_raw(i);
        };
        auto value = [&](int knot, int d) { return val[(size_t)knot * dims + d]; };

        if (curve == Curve::CatmullRom) {
            for (int i = 0; i < K; i++) {
                if (rest[(size_t)i]) continue;
                // Neighbours, across the loop's seam where there is one.
                const bool has_prev = i > 0 ? joined(i - 1) : _closed && joined(n - 1);
                const bool has_next = i < K - 1 ? joined(i) : _closed && joined(0);
                const int pk = i > 0 ? i - 1 : n - 1, nk = i < K - 1 ? i + 1 : 1;
                const double pu = i > 0 ? _u[(size_t)pk] : _u[(size_t)pk] - period;
                const double nu = i < K - 1 ? _u[(size_t)nk] : _u[(size_t)nk] + period;
                const double ps = i > 0 ? 1.0 : sign, ns = i < K - 1 ? 1.0 : sign;
                if (!has_prev && !has_next) continue;
                const double d0 = _u[(size_t)i] - pu, d1 = nu - _u[(size_t)i];
                double v0 = 0.0, v1 = 0.0, dot = 0.0, mm = 0.0;
                for (int d = 0; d < dims; d++) {
                    const double s0 = has_prev ? (value(i, d) - ps * value(pk, d)) / d0 : 0.0;
                    const double s1 = has_next ? (ns * value(nk, d) - value(i, d)) / d1 : 0.0;
                    const double m = has_prev && has_next ? (s0 * d1 + s1 * d0) / (d0 + d1)
                                     : has_prev ? s0 : s1;
                    out[(size_t)i * dims + d] = (1.0 - p.motion.tension) * m;
                    v0 += s0 * s0;
                    v1 += s1 * s1;
                    dot += s0 * s1;
                    mm += out[(size_t)i * dims + d] * out[(size_t)i * dims + d];
                }
                // On the whole vector, so a turned path moves as a turned path:
                // no faster than 3x the slower chord, easing off smoothly as the
                // path doubles back.
                if (!monotone || !has_prev || !has_next) continue;
                const double cap = 3.0 * std::sqrt(std::min(v0, v1));
                const double cosine = v0 > 0.0 && v1 > 0.0 ? dot / std::sqrt(v0 * v1) : 1.0;
                const double scale = std::clamp(1.0 + cosine, 0.0, 1.0) *
                                     (mm > cap * cap ? cap / std::sqrt(mm) : 1.0);
                for (int d = 0; d < dims; d++) out[(size_t)i * dims + d] *= scale;
            }
            return;
        }

        // Spline: a loop joined all round with no stop is one cyclic system.
        std::vector<double> y, m;
        if (_closed) {
            int start = -1;
            for (int i = 0; i < n && start < 0; i++)
                if (rest[(size_t)i] || !joined(i > 0 ? i - 1 : n - 1)) start = i;
            if (start < 0) {
                for (int d = 0; d < dims; d++) {
                    std::vector<double> a((size_t)n), b((size_t)n), c((size_t)n), r((size_t)n);
                    auto h = [&](int j) { return _u[(size_t)j + 1] - _u[(size_t)j]; };
                    auto next_val = [&](int j) { return j + 1 < n ? value(j + 1, d) : sign * value(0, d); };
                    for (int j = 0; j < n; j++) {
                        const int pj = j > 0 ? j - 1 : n - 1;
                        const double dp = j > 0 ? (value(j, d) - value(pj, d)) / h(pj)
                                                : (value(0, d) - sign * value(n - 1, d)) / h(n - 1);
                        const double dn = (next_val(j) - value(j, d)) / h(j);
                        a[(size_t)j] = 1.0 / h(pj);
                        c[(size_t)j] = 1.0 / h(j);
                        b[(size_t)j] = 2.0 * (a[(size_t)j] + c[(size_t)j]);
                        r[(size_t)j] = 3.0 * (dp / h(pj) + dn / h(j));
                    }
                    if (n == 2) {
                        // Both neighbours of each knot are the other one.
                        const double m00 = b[0], m01 = c[0] + sign * a[0];
                        const double m10 = a[1] + sign * c[1], m11 = b[1];
                        const double det = m00 * m11 - m01 * m10;
                        if (std::fabs(det) > 1e-300) {
                            r = {(r[0] * m11 - m01 * r[1]) / det, (m00 * r[1] - m10 * r[0]) / det};
                        }
                    } else {
                        cyclic(a, b, c, sign * c[(size_t)n - 1], sign * a[0], r);
                    }
                    for (int j = 0; j < n; j++) out[(size_t)j * dims + d] = r[(size_t)j];
                    out[(size_t)n * dims + d] = sign * r[0];
                }
                return;
            }
            // Unrolled from a stop or a break, once round.
            std::vector<Elem> seq;
            for (int i = start; i <= n; i++) seq.push_back({i, _u[(size_t)i], 1.0});
            for (int i = 1; i <= start; i++) seq.push_back({i, _u[(size_t)i] + period, sign});
            auto seq_joined = [&](size_t j) {
                const int a = seq[j].knot == n ? 0 : seq[j].knot;
                return joined(a);
            };
            size_t j = 0;
            while (j + 1 < seq.size()) {
                if (!seq_joined(j)) { j++; continue; }
                size_t e = j + 1;
                while (e + 1 < seq.size() && seq_joined(e) && !rest[(size_t)seq[e].knot]) e++;
                std::vector<Elem> piece(seq.begin() + (ptrdiff_t)j, seq.begin() + (ptrdiff_t)e + 1);
                for (int d = 0; d < dims; d++) {
                    y.clear();
                    for (const Elem& el : piece) y.push_back(el.sign * value(el.knot, d));
                    spline_piece(piece, y, rest[(size_t)piece.front().knot] != 0,
                                 rest[(size_t)piece.back().knot] != 0, m);
                    for (size_t q = 0; q < piece.size(); q++)
                        out[(size_t)piece[q].knot * dims + d] = piece[q].sign * m[q];
                }
                j = e;
            }
            if (start > 0)
                for (int d = 0; d < dims; d++) out[(size_t)d] = sign * out[(size_t)n * dims + d];
            return;
        }
        int i = 0;
        while (i + 1 < K) {
            if (!joined(i)) { i++; continue; }
            int e = i + 1;
            while (e + 1 < K && joined(e) && !rest[(size_t)e]) e++;
            std::vector<Elem> piece;
            for (int q = i; q <= e; q++) piece.push_back({q, _u[(size_t)q], 1.0});
            for (int d = 0; d < dims; d++) {
                y.clear();
                for (const Elem& el : piece) y.push_back(value(el.knot, d));
                spline_piece(piece, y, rest[(size_t)i] != 0, rest[(size_t)e] != 0, m);
                for (size_t q = 0; q < piece.size(); q++) out[(size_t)piece[q].knot * dims + d] = m[q];
            }
            i = e;
        }
    };
    auto always = [](int) { return true; };
    auto aimed = [&](int i) { return key(i).aim && key(i + 1).aim; };
    auto same_projection = [&](int i) {
        return p.lens_at(_src[(size_t)i]).projection == p.lens_at(_src[(size_t)i + 1]).projection;
    };
    solve(3, _pos, _dpos, always, 1.0, true);
    solve(4, _rot, _drot, always, rot_sign, false);
    solve(3, _tgt, _dtgt, aimed, 1.0, true);
    solve(1, _roll, _droll, aimed, 1.0, false);
    solve(1, _logf, _dfocal, same_projection, 1.0, true);

    // Arc length against the parameter, for constant speed and the readout.
    const int per = 48;
    const double u0 = _u.front(), u1 = _u.back();
    _cum_u.push_back(u0);
    _cum_len.push_back(0.0);
    const int steps = std::max(1, (K - 1) * per);
    double last[3] = {_pos[0], _pos[1], _pos[2]};
    for (int s = 1; s <= steps && u1 > u0; s++) {
        const double u = u0 + (u1 - u0) * s / steps;
        const CameraState c = eval(u);
        const double dx = c.pos[0] - last[0], dy = c.pos[1] - last[1],
                     dz = c.pos[2] - last[2];
        _length += std::sqrt(dx*dx + dy*dy + dz*dz);
        for (int d = 0; d < 3; d++) last[d] = c.pos[d];
        _cum_u.push_back(u);
        _cum_len.push_back(_length);
    }
}

double Trajectory::warp(double t) const {
    if (_knots < 2) return _u.empty() ? 0.0 : _u[0];
    const double t0 = _t.front(), t1 = _t.back();
    // The parameter is the time itself unless the speed is held constant.
    if (_u == _t) return std::clamp(t, t0, t1);
    if (t <= t0 || t1 <= t0) return _u.front();
    if (t >= t1) return _u.back();
    double f = (t - t0) / (t1 - t0);
    if (_p.motion.ease && !_closed) f = smoothstep(f);
    if (!(_length > 1e-12)) return _u.front() + f * (_u.back() - _u.front());
    const double s = f * _length;
    const auto it = std::lower_bound(_cum_len.begin(), _cum_len.end(), s);
    if (it == _cum_len.begin()) return _cum_u.front();
    if (it == _cum_len.end()) return _cum_u.back();
    const size_t j = (size_t)(it - _cum_len.begin());
    const double l0 = _cum_len[j - 1], l1 = _cum_len[j];
    const double g = l1 > l0 ? (s - l0) / (l1 - l0) : 0.0;
    return _cum_u[j - 1] + g * (_cum_u[j] - _cum_u[j - 1]);
}

CameraState Trajectory::at(double t) const { return eval(warp(t)); }

CameraState Trajectory::eval(double u) const {
    CameraState c;
    const std::vector<Keyframe>& k = _p.keys;
    if (k.empty()) return c;
    if (_knots == 1) {
        for (int d = 0; d < 3; d++) c.pos[d] = k[0].pos[d];
        for (int d = 0; d < 4; d++) c.rot[d] = k[0].rot[d];
        c.lens = _p.lens_at(0);
        return c;
    }
    double f;
    const int i = segment_of(_u, u, f);
    const int ka = _src[(size_t)i], kb = _src[(size_t)i + 1];
    const Keyframe& a = k[(size_t)ka];
    const Keyframe& b = k[(size_t)kb];
    const double du = _u[(size_t)i + 1] - _u[(size_t)i];
    const bool lines = _p.motion.curve == Curve::Linear;
    // An end that eases in straight lines still has to start from rest.
    double v = f;
    if (lines && _p.motion.ease && !_p.motion.constant_speed && !_closed &&
        (i == 0 || i + 2 == _knots)) {
        const bool first = i == 0, last = i + 2 == _knots;
        if (first && last) v = smoothstep(f);
        else if (first) v = f * f * (2.0 - f);   // rest at 0, full speed at 1
        else if (last) v = f * (1.0 + f - f * f);
    }
    auto channel = [&](const std::vector<double>& y, const std::vector<double>& m, int dims,
                       int d) {
        const double p0 = y[(size_t)i * dims + d], p1 = y[(size_t)(i + 1) * dims + d];
        if (lines) return p0 + (p1 - p0) * v;
        return hermite(p0, p1, m[(size_t)i * dims + d] * du,
                       m[(size_t)(i + 1) * dims + d] * du, f);
    };

    for (int d = 0; d < 3; d++) c.pos[d] = channel(_pos, _dpos, 3, d);

    if (a.aim && b.aim) {
        double tgt[3];
        for (int d = 0; d < 3; d++) tgt[d] = channel(_tgt, _dtgt, 3, d);
        const double roll = channel(_roll, _droll, 1, 0);
        aim_rotation(c.pos, tgt, _p.up, roll, c.rot);
    } else if (!lines) {
        for (int d = 0; d < 4; d++) c.rot[d] = channel(_rot, _drot, 4, d);
        normalize4(c.rot);
    } else {
        slerp(&_rot[(size_t)i * 4], &_rot[(size_t)(i + 1) * 4], v, c.rot);
    }

    // The projection and the distortion model cannot be blended, so they
    // change at the key; the zoom and the coefficients glide.
    const Lens& la = _p.lens_at(ka);
    const Lens& lb = _p.lens_at(kb);
    c.lens = f >= 1.0 ? lb : la;
    if (la.projection == lb.projection) {
        c.lens.focal = std::exp(channel(_logf, _dfocal, 1, 0));
        if (la.tier == lb.tier)
            for (int d = 0; d < kLensCoeffs; d++)
                c.lens.dist[d] = (float)(la.dist[d] + (lb.dist[d] - la.dist[d]) * smoothstep(f));
    }
    return c;
}

std::vector<double> Trajectory::sample_path(int n) const {
    std::vector<double> out;
    if (_knots == 0 || n < 2) return out;
    const double u0 = _u.front(), u1 = _u.back();
    out.reserve((size_t)n * 3);
    for (int s = 0; s < n; s++) {
        const CameraState c = eval(u0 + (u1 - u0) * s / (n - 1));
        out.insert(out.end(), c.pos, c.pos + 3);
    }
    return out;
}

}  // namespace gui::render
