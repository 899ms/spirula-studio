// EditSession.cpp -- see EditSession.h. The panel is EditPanel.cpp.

#include "app/gui/edit/EditSession.h"

#include "app/gui/ViewportPanel.h"
#include "i18n/Message.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

EditSession::~EditSession() { close(); }

void EditSession::open(std::unique_ptr<EditDoc> doc, ViewportPanel* panel) {
    close();
    _doc = std::move(doc);
    _panel = panel;
    if (_panel) _panel->set_interactor(this);
    _opt = SelectOptions{};
    _opt.by_extent = _doc && _doc->kind() == EditDoc::Kind::Splats;
    _tool.set_id(ToolId::Box);
    if (_doc) _doc->mark_geometry_dirty();
}

void EditSession::close() {
    if (_panel) _panel->set_interactor(nullptr);
    if (_doc) _doc->revert_display();
    if (_panel) _panel->invalidate();
    _panel = nullptr;
    _doc.reset();
    _grid.clear();
    _spacing = 0.0f;
    _occ.clear();
    _have_last = false;
    _tool.set_id(ToolId::Navigate);
    _status.clear();
}

std::vector<std::string> EditSession::drain_log() {
    std::vector<std::string> out;
    out.swap(_log);
    return out;
}

void EditSession::note(const std::string& s) {
    _log.push_back(s);
    if (_log.size() > 200) _log.erase(_log.begin(), _log.begin() + 100);
}

bool EditSession::view(ViewProjection& out) const {
    if (!_panel) return false;
    float x, y, w, h;
    _panel->image_rect(x, y, w, h);
    if (w < 8.0f || h < 8.0f) return false;
    out.W = (int)w;
    out.H = (int)h;
    _panel->view_camera(out.W, out.H, out.w2c, out.fx, out.fy,
                        out.camera_model, out.eye);
    out.cx = 0.5f * (float)out.W;
    out.cy = 0.5f * (float)out.H;
    return true;
}

Combine EditSession::combine_now(bool shift, bool ctrl) const {
    if (shift && ctrl) return Combine::Intersect;
    if (shift) return Combine::Add;
    if (ctrl) return Combine::Subtract;
    return (Combine)_combine;
}


// ---------------------------------------------------------------------------
// Viewport input
// ---------------------------------------------------------------------------

bool EditSession::on_viewport_input(const ViewportInput& in) {
    if (!_doc) return false;
    ShapeStroke s;
    bool consumed = false;
    if (_tool.update(in, s, consumed)) {
        if (_tool.id() == ToolId::Piece) select_component_under(s.pts[0], s.pts[1]);
        else apply_stroke(s, in);
    }
    return consumed;
}

void EditSession::draw_viewport_overlay(const ViewportOverlay& v) {
    if (!_doc) return;
    _tool.draw_overlay(v.dl, ImVec2(v.x, v.y));
}

void EditSession::apply_stroke(const ShapeStroke& s, const ViewportInput& in) {
    ViewProjection vp;
    if (!view(vp)) return;
    Stencil st;
    rasterize_shape(s, vp.W, vp.H, st);

    if (_opt.front_only) {
        if (_occ_dirty || std::memcmp(_occ_pose, vp.w2c, sizeof _occ_pose) != 0) {
            _occ.build(*_doc, vp);
            std::memcpy(_occ_pose, vp.w2c, sizeof _occ_pose);
            _occ_alive = _doc->alive_count();
            _occ_dirty = false;
        }
    }

    std::vector<uint8_t> w;
    _last_result = select_by_stencil(*_doc, vp, st, _opt,
                                     _opt.front_only ? &_occ : nullptr, w);
    _last_shape = s;
    _last_view = vp;
    _last_combine = combine_now(in.shift, in.ctrl);
    _before_last = _doc->sel().weights();
    _have_last = true;

    Selection tmp;
    tmp.assign(_before_last);
    tmp.combine(w.data(), _last_combine, _doc->alive());
    run_select(tmp.weights(), msg::op_select);
}

void EditSession::reapply_depth() {
    if (!_have_last || !_doc) return;
    // The last selection is replaced rather than stacked: dragging a slider
    // must not leave a hundred entries in the history.
    if (_doc->can_undo()) _doc->undo();
    Stencil st;
    rasterize_shape(_last_shape, _last_view.W, _last_view.H, st);
    std::vector<uint8_t> w;
    _last_result = select_by_stencil(*_doc, _last_view, st, _opt,
                                     _opt.front_only ? &_occ : nullptr, w);
    Selection tmp;
    tmp.assign(_before_last);
    tmp.combine(w.data(), _last_combine, _doc->alive());
    run_select(tmp.weights(), msg::op_select);
}

void EditSession::run_select(std::vector<uint8_t> w,
                             const spirula::i18n::Msg& name) {
    if (!_doc) return;
    _doc->run(make_select_op(*_doc, std::move(w), name));
}


// ---------------------------------------------------------------------------
// Neighbourhood operations
// ---------------------------------------------------------------------------

// The neighbour radius everything below works at: a multiple of how far
// apart these elements actually are, measured once.
float EditSession::reach() {
    if (!_doc) return 0.0f;
    if (!(_spacing > 0.0f)) _spacing = _grid.measure_spacing(*_doc);
    return _spacing * _radius_mul;
}

void EditSession::ensure_grid() {
    if (!_doc) return;
    const float r = reach();
    if (_grid.built() && std::fabs(_grid.cell() - r) < 1e-9f * std::max(r, 1.0f))
        return;
    _grid.build(*_doc, r);
}

void EditSession::grow_shrink(bool grow) {
    if (!_doc) return;
    const float r = reach();
    ensure_grid();
    std::vector<uint8_t> w = _doc->sel().weights();
    if (grow) _grid.grow(w, r, _doc->alive(), _doc->count());
    else      _grid.shrink(w, r, _doc->alive(), _doc->count());
    _have_last = false;
    run_select(std::move(w), grow ? msg::op_grow : msg::op_shrink);
}

void EditSession::select_component_under(float px, float py) {
    if (!_doc) return;
    ViewProjection vp;
    if (!view(vp)) return;
    const int64_t hit = pick_element(*_doc, vp, px, py, 12.0f);
    if (hit < 0) return;
    const float r = reach();
    ensure_grid();
    std::vector<int32_t> label;
    std::vector<int64_t> sizes;
    _grid.components(r, _doc->alive(), _doc->count(), label, sizes);
    const int32_t want = label[(size_t)hit];
    if (want < 0) return;
    std::vector<uint8_t> w((size_t)_doc->count(), 0);
    for (int64_t i = 0; i < _doc->count(); i++)
        if (label[(size_t)i] == want) w[(size_t)i] = 255;
    Selection tmp;
    tmp.assign(_doc->sel().weights());
    tmp.combine(w.data(), (Combine)_combine, _doc->alive());
    _have_last = false;
    run_select(tmp.weights(), msg::op_select_piece);
}

void EditSession::keep_largest_components(int keep) {
    if (!_doc) return;
    const float r = reach();
    ensure_grid();
    std::vector<int32_t> label;
    std::vector<int64_t> sizes;
    _grid.components(r, _doc->alive(), _doc->count(), label, sizes);
    std::vector<int32_t> order((size_t)sizes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
        return sizes[(size_t)a] > sizes[(size_t)b];
    });
    std::vector<uint8_t> big((size_t)sizes.size(), 0);
    for (int i = 0; i < keep && i < (int)order.size(); i++)
        big[(size_t)order[(size_t)i]] = 1;
    std::vector<uint8_t> w((size_t)_doc->count(), 0);
    for (int64_t i = 0; i < _doc->count(); i++) {
        const int32_t l = label[(size_t)i];
        if (l >= 0 && !big[(size_t)l]) w[(size_t)i] = 255;
    }
    _have_last = false;
    run_select(std::move(w), msg::op_select_floaters);
}


// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void EditSession::poll() {
    if (!_doc) return;
    handle_keys();
    // The occlusion buffer belongs to one camera and one live set; either
    // moving invalidates it, and rebuilding is the next selection's business
    // rather than this frame's.
    ViewProjection vp;
    if (view(vp) && std::memcmp(_occ_pose, vp.w2c, sizeof _occ_pose) != 0)
        _occ_dirty = true;
    if (_doc->alive_count() != _occ_alive) _occ_dirty = true;
    if (_doc->publish() && _panel) _panel->invalidate();
}

void EditSession::save_to(int target, const std::string& path) {
    if (!_doc || path.empty()) return;
    try {
        _doc->save(target, path);
        _doc->mark_saved();
        _status = spirula::i18n::format(msg::saved_to, {path});
        _status_err = false;
        note(_status);
    } catch (const std::exception& e) {
        _status = spirula::i18n::format(msg::save_failed, {std::string(e.what())});
        _status_err = true;
        note(_status);
    }
}

}  // namespace gui
