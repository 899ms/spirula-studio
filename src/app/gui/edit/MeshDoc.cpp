// MeshDoc.cpp -- see MeshDoc.h.

#include "app/gui/edit/MeshDoc.h"

#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

constexpr unsigned char kTint[3] = {255, 108, 13};

bool textured(const meshing::MeshData& m) {
    return m.UV.size() == m.V.size() && m.tex_width > 0 && m.tex_height > 0 &&
           m.texture.size() >= (size_t)m.tex_width * m.tex_height * 3;
}

std::array<unsigned char, 3> sample_texture(const meshing::MeshData& m,
                                            size_t i) {
    const float u = m.UV[i][0], v = m.UV[i][1];
    const int x = std::clamp((int)(u * (float)m.tex_width), 0, m.tex_width - 1);
    const int y = std::clamp((int)(v * (float)m.tex_height), 0, m.tex_height - 1);
    const size_t o = ((size_t)y * m.tex_width + x) * 3;
    return {m.texture[o], m.texture[o + 1], m.texture[o + 2]};
}

}  // namespace


MeshDoc::MeshDoc(meshing::MeshData mesh, const std::string& source,
                 const float to_view[12],
                 std::function<void(const meshing::MeshData&, const float*)> show)
    : _m(std::move(mesh)), _show(std::move(show)) {
    if (to_view) for (int i = 0; i < 12; i++) _t2n[i] = to_view[i];
    const int64_t n = (int64_t)_m.V.size();
    std::vector<float> pos((size_t)n * 3);
    for (int64_t i = 0; i < n; i++) {
        const auto& p = _m.V[(size_t)i];
        for (int r = 0; r < 3; r++)
            pos[(size_t)i * 3 + r] = _t2n[r*4+0]*p[0] + _t2n[r*4+1]*p[1] +
                                     _t2n[r*4+2]*p[2] + _t2n[r*4+3];
    }
    _live_faces = (int64_t)_m.F.size();
    _edges.reserve(_m.F.size() * 6);
    for (const auto& f : _m.F)
        for (int k = 0; k < 3; k++) {
            _edges.push_back(f[k]);
            _edges.push_back(f[(k + 1) % 3]);
        }

    _display.V = _m.V;
    _display.N = _m.N;
    const bool has_uv = textured(_m);
    _display.C.resize(_m.V.size());
    for (size_t i = 0; i < _m.V.size(); i++) {
        if (has_uv) _display.C[i] = sample_texture(_m, i);
        else if (_m.C.size() == _m.V.size()) _display.C[i] = _m.C[i];
        else _display.C[i] = {184, 184, 184};
    }
    set_source(source);
    add_layer(msg::elem_vertex, n, std::move(pos));
}

int64_t MeshDoc::pick(const ViewProjection& view, float px, float py) const {
    float ro[3], rd[3];
    if (!view.unproject(px, py, ro, rd)) return -1;
    const float* P = positions();
    const uint8_t* live = alive();
    int64_t best = -1;
    float best_t = 1e30f;
    // Moller-Trumbore over the live faces. A click is not a frame, so brute
    // force is the right amount of machinery for it.
    for (const auto& f : _m.F) {
        if (!(live[f[0]] && live[f[1]] && live[f[2]])) continue;
        const float* a = P + (size_t)f[0] * 3;
        const float* b = P + (size_t)f[1] * 3;
        const float* c = P + (size_t)f[2] * 3;
        float e1[3], e2[3], pv[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = b[k] - a[k];
            e2[k] = c[k] - a[k];
        }
        pv[0] = rd[1]*e2[2] - rd[2]*e2[1];
        pv[1] = rd[2]*e2[0] - rd[0]*e2[2];
        pv[2] = rd[0]*e2[1] - rd[1]*e2[0];
        const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
        if (std::fabs(det) < 1e-20f) continue;
        const float inv = 1.0f / det;
        float tv[3];
        for (int k = 0; k < 3; k++) tv[k] = ro[k] - a[k];
        const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
        if (u < 0.0f || u > 1.0f) continue;
        float qv[3];
        qv[0] = tv[1]*e1[2] - tv[2]*e1[1];
        qv[1] = tv[2]*e1[0] - tv[0]*e1[2];
        qv[2] = tv[0]*e1[1] - tv[1]*e1[0];
        const float v = (rd[0]*qv[0] + rd[1]*qv[1] + rd[2]*qv[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
        if (t <= 1e-6f || t >= best_t) continue;
        best_t = t;
        best = f[0];
    }
    return best;
}

void MeshDoc::publish_impl(bool geometry) {
    if (!_show) return;
    const uint8_t* alive = this->alive();
    const uint8_t* sel = this->sel().data();
    const bool has_uv = textured(_m);
    for (size_t i = 0; i < _m.V.size(); i++) {
        std::array<unsigned char, 3> base;
        if (has_uv) base = sample_texture(_m, i);
        else if (_m.C.size() == _m.V.size()) base = _m.C[i];
        else base = {184, 184, 184};
        _display.C[i] = sel[i] ? std::array<unsigned char, 3>{kTint[0], kTint[1],
                                                              kTint[2]}
                               : base;
    }
    if (geometry || _display.F.empty()) {
        _display.F.clear();
        _display.F.reserve(_m.F.size());
        for (const auto& f : _m.F)
            if (alive[f[0]] && alive[f[1]] && alive[f[2]]) _display.F.push_back(f);
        _live_faces = (int64_t)_display.F.size();
    }
    _show(_display, _t2n);
}

void MeshDoc::revert_display() {
    if (_show) _show(_m, _t2n);
}

std::vector<SaveTarget> MeshDoc::save_targets() const {
    return {{&msg::target_mesh_ply, ".ply", false},
            {&msg::target_mesh_obj, ".obj", false},
            {&msg::target_mesh_glb, ".glb", false},
            {&msg::target_mesh_stl, ".stl", false}};
}

std::string MeshDoc::default_save_path(int) const { return source_path(); }

void MeshDoc::save(int target, const std::string& path) {
    static const char* kFmt[] = {"ply", "obj", "glb", "stl"};
    const int t = std::clamp(target, 0, 3);
    const std::vector<uint8_t>& keep = alive_of(0);

    // Drop the faces a deleted vertex took with it, then the vertices nothing
    // refers to any more -- a file full of orphans is not what was asked for.
    meshing::MeshData out;
    const uint8_t* alive = keep.data();
    std::vector<int> remap(_m.V.size(), -1);
    out.F.reserve(_m.F.size());
    for (const auto& f : _m.F) {
        if (!(alive[f[0]] && alive[f[1]] && alive[f[2]])) continue;
        std::array<int, 3> g{};
        for (int k = 0; k < 3; k++) {
            int& r = remap[(size_t)f[k]];
            if (r < 0) {
                r = (int)out.V.size();
                out.V.push_back(_m.V[(size_t)f[k]]);
                if (_m.N.size() == _m.V.size()) out.N.push_back(_m.N[(size_t)f[k]]);
                if (_m.C.size() == _m.V.size()) out.C.push_back(_m.C[(size_t)f[k]]);
                if (_m.UV.size() == _m.V.size()) out.UV.push_back(_m.UV[(size_t)f[k]]);
            }
            g[k] = r;
        }
        out.F.push_back(g);
    }
    out.tex_width = _m.tex_width;
    out.tex_height = _m.tex_height;
    out.texture = _m.texture;

    meshing::MeshColorMode mode = meshing::MeshColorMode::None;
    if (!out.UV.empty() && !out.texture.empty())
        mode = meshing::MeshColorMode::Texture;
    else if (!out.C.empty())
        mode = meshing::MeshColorMode::Vertex;
    const meshing::MeshFormatSpec spec = meshing::parse_one_mesh_format(kFmt[t]);
    if (!meshing::check_export_support(spec, mode).empty()) {
        mode = out.C.empty() ? meshing::MeshColorMode::None
                             : meshing::MeshColorMode::Vertex;
        if (!meshing::check_export_support(spec, mode).empty())
            mode = meshing::MeshColorMode::None;
    }
    meshing::write_mesh(out, mode, spec,
                        meshing::mesh_output_strip_ext(path), false);
}

}  // namespace gui
