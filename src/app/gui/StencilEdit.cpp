// StencilEdit.cpp -- see StencilEdit.h.

#include "app/gui/StencilEdit.h"

#include "core/PolygonFill.h"

#include <algorithm>
#include <cmath>

namespace gui {

using Kind = app::MaskShape::Kind;

namespace {

constexpr size_t kMaxUndo = 64;

// Distance from (u, v) to segment a-b, in units of the stroke's half-width.
float stroke_distance(const app::MaskShape& s, size_t i, size_t j, float u, float v) {
    const float ax = s.pts[2 * i] / s.rx, ay = s.pts[2 * i + 1] / s.ry;
    const float dx = s.pts[2 * j] / s.rx - ax, dy = s.pts[2 * j + 1] / s.ry - ay;
    const float qx = u / s.rx - ax, qy = v / s.ry - ay;
    const float len2 = dx * dx + dy * dy;
    const float t = len2 > 0.0f ? std::clamp((qx * dx + qy * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(qx - t * dx, qy - t * dy);
}

}  // namespace

bool stencil_shape_from_stroke(const ShapeStroke& s, float cw, float ch, bool remove,
                               app::MaskShape& out) {
    out = app::MaskShape{};
    out.remove = remove;
    const std::vector<float>& p = s.pts;
    if (cw <= 0.0f || ch <= 0.0f || p.size() < 2) return false;
    switch (s.kind) {
        case ShapeKind::Box:
        case ShapeKind::Ellipse: {
            if (p.size() < 4) return false;
            const float ax = std::min(p[0], p[2]) / cw, bx = std::max(p[0], p[2]) / cw;
            const float ay = std::min(p[1], p[3]) / ch, by = std::max(p[1], p[3]) / ch;
            // Two pixels either way: a click is not a shape.
            if ((bx - ax) * cw < 2.0f || (by - ay) * ch < 2.0f) return false;
            if (s.kind == ShapeKind::Box) {
                out.kind = Kind::Rect;
                out.cx = ax; out.cy = ay; out.rx = bx; out.ry = by;
            } else {
                out.kind = Kind::Ellipse;
                out.cx = 0.5f * (ax + bx);
                out.cy = 0.5f * (ay + by);
                out.rx = 0.5f * (bx - ax);
                out.ry = 0.5f * (by - ay);
            }
            return true;
        }
        case ShapeKind::Lasso:
        case ShapeKind::Polygon:
            if (p.size() < 6) return false;
            out.kind = Kind::Path;
            out.pts.resize(p.size() - p.size() % 2);
            for (size_t i = 0; i + 1 < out.pts.size(); i += 2) {
                out.pts[i] = p[i] / cw;
                out.pts[i + 1] = p[i + 1] / ch;
            }
            return true;
        case ShapeKind::Brush: {
            const float r = std::max(s.brush_radius, 0.5f);
            out.kind = Kind::Stroke;
            out.rx = r / cw;
            out.ry = r / ch;
            // A drag samples every frame; a point closer than a quarter of the
            // radius to the last kept one changes nothing the fill can see.
            float lx = p[0], ly = p[1];
            out.pts = {lx / cw, ly / ch};
            for (size_t i = 2; i + 1 < p.size(); i += 2) {
                const bool last = i + 2 >= p.size();
                if (!last && std::hypot(p[i] - lx, p[i + 1] - ly) < 0.25f * r) continue;
                lx = p[i];
                ly = p[i + 1];
                out.pts.insert(out.pts.end(), {lx / cw, ly / ch});
            }
            return true;
        }
    }
    return false;
}

int stencil_handles(const app::MaskShape& s, float u[3], float v[3]) {
    switch (s.kind) {
        case Kind::Ellipse:
            u[0] = s.cx; v[0] = s.cy;
            u[1] = s.cx + s.rx; v[1] = s.cy;
            u[2] = s.cx; v[2] = s.cy + s.ry;
            return 3;
        case Kind::Rect:
            u[0] = 0.5f * (s.cx + s.rx); v[0] = 0.5f * (s.cy + s.ry);
            u[1] = s.cx; v[1] = s.cy;
            u[2] = s.rx; v[2] = s.ry;
            return 3;
        case Kind::Path:
        case Kind::Stroke:
            return 0;
    }
    return 0;
}

bool stencil_contains(const app::MaskShape& s, float u, float v) {
    switch (s.kind) {
        case Kind::Path:
            return polyfill::contains(s.pts.data(), s.pts.size() / 2, u, v);
        case Kind::Stroke: {
            const size_t n = s.pts.size() / 2;
            if (n == 0 || s.rx <= 0.0f || s.ry <= 0.0f) return false;
            for (size_t i = 0; i < n; i++)
                if (stroke_distance(s, i, i + 1 < n ? i + 1 : i, u, v) <= 1.0f) return true;
            return false;
        }
        case Kind::Ellipse: {
            if (s.rx <= 0.0f || s.ry <= 0.0f) return false;
            const float du = (u - s.cx) / s.rx, dv = (v - s.cy) / s.ry;
            return du * du + dv * dv <= 1.0f;
        }
        case Kind::Rect:
            return u >= std::min(s.cx, s.rx) && u <= std::max(s.cx, s.rx) &&
                   v >= std::min(s.cy, s.ry) && v <= std::max(s.cy, s.ry);
    }
    return false;
}

void stencil_move(app::MaskShape& s, float du, float dv) {
    if (s.kind == Kind::Path || s.kind == Kind::Stroke) {
        for (size_t i = 0; i + 1 < s.pts.size(); i += 2) {
            s.pts[i] += du;
            s.pts[i + 1] += dv;
        }
        return;
    }
    s.cx += du;
    s.cy += dv;
    if (s.kind == Kind::Rect) {
        s.rx += du;
        s.ry += dv;
    }
}

void stencil_move_handle(app::MaskShape& s, int handle, float u, float v) {
    if (s.kind == Kind::Ellipse) {
        if (handle == 0) { s.cx = u; s.cy = v; }
        else if (handle == 1) s.rx = std::max(0.005f, std::fabs(u - s.cx));
        else s.ry = std::max(0.005f, std::fabs(v - s.cy));
        return;
    }
    if (s.kind != Kind::Rect) return;
    if (handle == 0) {
        const float w = s.rx - s.cx, h = s.ry - s.cy;
        s.cx = u - w * 0.5f;
        s.cy = v - h * 0.5f;
        s.rx = s.cx + w;
        s.ry = s.cy + h;
    } else if (handle == 1) { s.cx = u; s.cy = v; }
    else { s.rx = u; s.ry = v; }
}

void StencilHistory::push(const std::vector<app::MaskShape>& before) {
    _undo.push_back(before);
    if (_undo.size() > kMaxUndo) _undo.erase(_undo.begin());
    _redo.clear();
}

bool StencilHistory::undo(std::vector<app::MaskShape>& shapes) {
    if (_undo.empty()) return false;
    _redo.push_back(std::move(shapes));
    shapes = std::move(_undo.back());
    _undo.pop_back();
    return true;
}

bool StencilHistory::redo(std::vector<app::MaskShape>& shapes) {
    if (_redo.empty()) return false;
    _undo.push_back(std::move(shapes));
    shapes = std::move(_redo.back());
    _redo.pop_back();
    return true;
}

void StencilHistory::clear() {
    _undo.clear();
    _redo.clear();
}

}  // namespace gui
