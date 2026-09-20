// SplatDoc.cpp -- see SplatDoc.h.

#include "app/gui/edit/SplatDoc.h"

#include "engine/Engine.h"
#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

// Below this the projection's own alpha cull drops the Gaussian before it
// reaches a tile, so a deleted one costs nothing to leave in place.
constexpr float kDeadOpacity = -30.0f;
// The DC band is (colour - 0.5) / C0, so a colour comes back this way.
constexpr float kSh0 = 0.28209479177387814f;
// Deliberately outside [0,1]: the view-dependent bands are still there, and a
// tint inside the displayable range vanishes under them wherever they are
// strong. Past the clamp, red saturates and blue pins off whatever the SH does.
constexpr float kTint[3] = {3.0f, 0.8f, -1.5f};

TorchTensorView tv(std::vector<float>& v, std::vector<int64_t> shape) {
    return {(uint64_t)(uintptr_t)v.data(), (uint32_t)sizeof(float),
            std::move(shape)};
}

}  // namespace


SplatDoc::SplatDoc(spirula::SplatCloud cloud, const std::string& source,
                   const float to_view[12], int slot, std::mutex* mu)
    : _c(std::move(cloud)), _slot(slot), _mu(mu) {
    const int64_t n = _c.num;
    const float scale = std::sqrt(to_view[0] * to_view[0] +
                                  to_view[4] * to_view[4] +
                                  to_view[8] * to_view[8]);
    std::vector<float> pos((size_t)n * 3);
    _radius.resize((size_t)n);
    for (int64_t i = 0; i < n; i++) {
        const float* m = &_c.means[(size_t)i * 3];
        for (int r = 0; r < 3; r++)
            pos[(size_t)i * 3 + r] = to_view[r * 4 + 0] * m[0] +
                                     to_view[r * 4 + 1] * m[1] +
                                     to_view[r * 4 + 2] * m[2] + to_view[r * 4 + 3];
        const float s = std::max(std::max(_c.scales[(size_t)i * 3],
                                          _c.scales[(size_t)i * 3 + 1]),
                                 _c.scales[(size_t)i * 3 + 2]);
        _radius[(size_t)i] = std::exp(std::min(s, 8.0f)) * scale;
    }
    _opacity.resize((size_t)n);
    _dc.resize((size_t)n * 3);
    init(n, std::move(pos), source);
}

const spirula::i18n::Msg& SplatDoc::element_name() const {
    return msg::elem_gaussian;
}

void SplatDoc::publish_impl(bool geometry) {
    if (_slot < 0 || !_mu) return;
    const int64_t n = count();
    const uint8_t* alive = this->alive();
    const uint8_t* sel = this->sel().data();
    for (int64_t i = 0; i < n; i++) {
        _opacity[(size_t)i] = alive[i] ? _c.opacities[(size_t)i] : kDeadOpacity;
        for (int k = 0; k < 3; k++) {
            const float base = _c.features_dc[(size_t)i * 3 + k];
            const float t = (kTint[k] - 0.5f) / kSh0;
            const float w = sel[i] * (1.0f / 255.0f);
            _dc[(size_t)i * 3 + k] = base + w * (t - base);
        }
    }
    std::lock_guard<std::mutex> lk(*_mu);
    if (geometry) engine_scene_update(_slot, "opacities", tv(_opacity, {n, 1}));
    engine_scene_update(_slot, "features_dc", tv(_dc, {n, 3}));
}

void SplatDoc::revert_display() {
    if (_slot < 0 || !_mu) return;
    const int64_t n = count();
    std::lock_guard<std::mutex> lk(*_mu);
    engine_scene_update(_slot, "opacities", tv(_c.opacities, {n, 1}));
    engine_scene_update(_slot, "features_dc", tv(_c.features_dc, {n, 3}));
}

std::vector<SaveTarget> SplatDoc::save_targets() const {
    return {{&msg::target_splat_ply, ".ply", false}};
}

std::string SplatDoc::default_save_path(int) const { return source_path(); }

void SplatDoc::save(int, const std::string& path) {
    spirula::write_splat_ply(_c, path, alive());
}

}  // namespace gui
