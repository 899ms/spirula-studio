// PointsDoc.cpp -- see PointsDoc.h.

#include "app/gui/edit/PointsDoc.h"

#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

constexpr uint8_t kTint[3] = {255, 108, 13};

}  // namespace


PointsDoc::PointsDoc(
    ParsedDataset ds, PostSplitCameras post, const std::string& source,
    const std::string& dataset_dir,
    std::function<void(const ParsedDataset&, const PostSplitCameras&)> show)
    : _ds(std::move(ds)), _post(std::move(post)), _dataset_dir(dataset_dir),
      _show(std::move(show)) {
    if (!_dataset_dir.empty()) _fmt = spirula::sparse_format_of(_dataset_dir);

    // The preview draws in the normalized frame, so the selection has to
    // project there too: train_to_normalized is stored the other way round.
    double A[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (_ds.train_frame_scale != 1.0f) {
        double T[16];
        for (int i = 0; i < 16; i++) T[i] = _ds.train_to_normalized[i];
        dsparse::invert_affine4x4(T, A);
    }
    const int64_t n = _ds.points.num();
    std::vector<float> pos((size_t)n * 3);
    for (int64_t i = 0; i < n; i++) {
        const double* p = &_ds.points.xyz[(size_t)i * 3];
        for (int r = 0; r < 3; r++)
            pos[(size_t)i * 3 + r] = (float)(A[r*4+0]*p[0] + A[r*4+1]*p[1] +
                                             A[r*4+2]*p[2] + A[r*4+3]);
    }
    _display = _ds;
    init(n, std::move(pos), source);
}

const spirula::i18n::Msg& PointsDoc::element_name() const {
    return msg::elem_point;
}

void PointsDoc::publish_impl(bool) {
    if (!_show) return;
    const int64_t n = count();
    const uint8_t* alive = this->alive();
    const uint8_t* sel = this->sel().data();
    ColmapPoints3D& out = _display.points;
    out.xyz.clear();
    out.rgb.clear();
    out.xyz.reserve((size_t)alive_count() * 3);
    out.rgb.reserve((size_t)alive_count() * 3);
    for (int64_t i = 0; i < n; i++) {
        if (!alive[i]) continue;
        for (int k = 0; k < 3; k++) out.xyz.push_back(_ds.points.xyz[(size_t)i*3+k]);
        for (int k = 0; k < 3; k++) {
            const uint8_t base = _ds.points.rgb.empty()
                                     ? (uint8_t)200
                                     : _ds.points.rgb[(size_t)i * 3 + k];
            out.rgb.push_back(sel[i] ? kTint[k] : base);
        }
    }
    _show(_display, _post);
}

void PointsDoc::revert_display() {
    if (_show) _show(_ds, _post);
}

std::vector<SaveTarget> PointsDoc::save_targets() const {
    std::vector<SaveTarget> t;
    switch (_fmt) {
        case spirula::SparseFormat::Colmap:
            t.push_back({&msg::target_colmap, "", true});
            break;
        case spirula::SparseFormat::Nerfstudio:
        case spirula::SparseFormat::Metashape:
            t.push_back({&msg::target_nerfstudio, "", true});
            break;
        default:
            break;
    }
    t.push_back({&msg::target_points_ply, ".ply", false});
    return t;
}

std::string PointsDoc::default_save_path(int target) const {
    const std::vector<SaveTarget> t = save_targets();
    if (target < 0 || target >= (int)t.size()) return {};
    return t[(size_t)target].folder ? _dataset_dir : source_path();
}

void PointsDoc::save(int target, const std::string& path) {
    const std::vector<SaveTarget> t = save_targets();
    if (target < 0 || target >= (int)t.size()) return;
    if (t[(size_t)target].folder) {
        std::vector<uint8_t> keep(_alive.begin(), _alive.end());
        spirula::sparse_write_filtered(path, keep);
        return;
    }
    spirula::write_ply_points(
        path, _ds.points.xyz.data(),
        _ds.points.rgb.empty() ? nullptr : _ds.points.rgb.data(),
        _ds.points.num(), alive());
}

}  // namespace gui
