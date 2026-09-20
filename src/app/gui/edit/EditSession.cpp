// EditSession.cpp -- see EditSession.h. The panel is EditPanel.cpp.

#include "app/gui/edit/EditSession.h"

#include "app/gui/ViewportPanel.h"
#include "i18n/Message.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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
    // Opening in Navigate: the first thing anyone does with a model they have
    // just opened is look at it from somewhere else.
    _tool.set_id(ToolId::Navigate);
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
    _last = LastAction{};
    _tool.set_id(ToolId::Navigate);
    _status.clear();
    _ask_overwrite = false;
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

void EditSession::set_layer(int i) {
    if (!_doc || i == _doc->layer()) return;
    _doc->set_layer(i);
    // Everything measured is measured per layer: a camera table and a point
    // cloud have nothing in common but their frame.
    _grid.clear();
    _spacing = 0.0f;
    _occ_dirty = true;
    _last = LastAction{};
    _doc->mark_display_dirty();
}


// ---------------------------------------------------------------------------
// Viewport input
// ---------------------------------------------------------------------------

bool EditSession::on_viewport_input(const ViewportInput& in) {
    if (!_doc) return false;
    ShapeStroke s;
    bool consumed = false;
    if (_tool.update(in, s, consumed)) {
        if (_tool.id() == ToolId::Piece) {
            _last.combine = combine_now(in.shift, in.ctrl);
            select_component_under(s.pts[0], s.pts[1]);
        } else {
            apply_stroke(s, in);
        }
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
    _last = LastAction{};
    _last.kind = LastAction::Stencil;
    _last.layer = _doc->layer();
    _last.before = _doc->sel().weights();
    _last.combine = combine_now(in.shift, in.ctrl);
    _last.shape = s;
    _last.view = vp;
    run_last(/*fresh=*/true);
}

void EditSession::reapply_last() {
    if (!_doc || _last.kind == LastAction::None) return;
    if (_last.layer != _doc->layer()) return;
    // The last selection is replaced rather than stacked: dragging a slider
    // must not leave a hundred entries in the history.
    if (_doc->can_undo()) _doc->undo();
    run_last(/*fresh=*/false);
}

void EditSession::run_last(bool fresh) {
    (void)fresh;
    if (!_doc || _last.kind == LastAction::None) return;
    std::vector<uint8_t> w((size_t)_doc->count(), 0);
    const spirula::i18n::Msg* name = &msg::op_select;

    switch (_last.kind) {
        case LastAction::Stencil: {
            Stencil st;
            rasterize_shape(_last.shape, _last.view.W, _last.view.H, st);
            if (_opt.front_only) {
                if (_occ_dirty ||
                    std::memcmp(_occ_pose, _last.view.w2c, sizeof _occ_pose) != 0) {
                    _occ.build(*_doc, _last.view);
                    std::memcpy(_occ_pose, _last.view.w2c, sizeof _occ_pose);
                    _occ_alive = _doc->alive_count();
                    _occ_dirty = false;
                }
            }
            _last_result = select_by_stencil(*_doc, _last.view, st, _opt,
                                             _opt.front_only ? &_occ : nullptr, w);
            break;
        }
        case LastAction::Grow:
        case LastAction::Shrink: {
            const float r = reach();
            ensure_grid();
            w = _last.before;
            if (_last.kind == LastAction::Grow)
                _grid.grow(w, r, _doc->alive(), _doc->count());
            else
                _grid.shrink(w, r, _doc->alive(), _doc->count());
            name = _last.kind == LastAction::Grow ? &msg::op_grow : &msg::op_shrink;
            // Grow and shrink are their own combine: they start from what is
            // already selected rather than meeting it.
            _doc->run(make_select_op(*_doc, std::move(w), *name));
            return;
        }
        case LastAction::Piece: {
            if (_last.seed < 0) break;
            std::vector<int32_t> label;
            std::vector<int64_t> sizes;
            components(label, sizes);
            const int32_t want = label[(size_t)_last.seed];
            if (want >= 0)
                for (int64_t i = 0; i < _doc->count(); i++)
                    if (label[(size_t)i] == want) w[(size_t)i] = 255;
            name = &msg::op_select_piece;
            break;
        }
        case LastAction::Floaters: {
            std::vector<int32_t> label;
            std::vector<int64_t> sizes;
            components(label, sizes);
            std::vector<int32_t> order((size_t)sizes.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
                return sizes[(size_t)a] > sizes[(size_t)b];
            });
            std::vector<uint8_t> big((size_t)sizes.size(), 0);
            for (int i = 0; i < std::max(1, _keep_components) &&
                            i < (int)order.size(); i++)
                big[(size_t)order[(size_t)i]] = 1;
            for (int64_t i = 0; i < _doc->count(); i++) {
                const int32_t l = label[(size_t)i];
                if (l >= 0 && !big[(size_t)l]) w[(size_t)i] = 255;
            }
            name = &msg::op_select_floaters;
            break;
        }
        default:
            return;
    }

    Selection tmp;
    tmp.assign(_last.before);
    tmp.combine(w.data(), _last.combine, _doc->alive());
    _doc->run(make_select_op(*_doc, tmp.weights(), *name));
}

void EditSession::run_select(std::vector<uint8_t> w,
                             const spirula::i18n::Msg& name) {
    if (!_doc) return;
    _last = LastAction{};
    _doc->run(make_select_op(*_doc, std::move(w), name));
}


// ---------------------------------------------------------------------------
// Neighbourhood operations
// ---------------------------------------------------------------------------

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

// A mesh says what is joined to what; a cloud has to be asked by distance,
// and a Gaussian's own extent is part of that answer.
void EditSession::components(std::vector<int32_t>& label,
                             std::vector<int64_t>& sizes) {
    int64_t pairs = 0;
    if (const int32_t* topo = _doc->topology(pairs)) {
        components_from_pairs(topo, pairs, _doc->alive(), _doc->count(), label,
                              sizes);
        return;
    }
    // Gaussians overlap by construction, so "these two touch" has to mean
    // they interpenetrate: at 1.0 a trained scene is one piece. What it buys
    // is the big sky splat, whose neighbours are as far off as they are large.
    constexpr float kOverlap = 0.5f;
    const float r = reach();
    ensure_grid();
    _grid.components(r, _doc->alive(), _doc->count(), label, sizes,
                     _doc->radii(), kOverlap);
}

void EditSession::grow_shrink(bool grow) {
    if (!_doc) return;
    _last = LastAction{};
    _last.kind = grow ? LastAction::Grow : LastAction::Shrink;
    _last.layer = _doc->layer();
    _last.before = _doc->sel().weights();
    run_last(/*fresh=*/true);
}

void EditSession::select_seed(int64_t seed) {
    if (!_doc) return;
    const Combine c = _last.combine;
    _last = LastAction{};
    _last.kind = LastAction::Piece;
    _last.layer = _doc->layer();
    _last.before = _doc->sel().weights();
    _last.combine = c;
    _last.seed = seed;
    run_last(/*fresh=*/true);
}

void EditSession::select_component_under(float px, float py) {
    if (!_doc) return;
    ViewProjection vp;
    if (!view(vp)) return;
    const int64_t hit = pick_element(*_doc, vp, px, py, 16.0f);
    if (hit < 0) {
        // Clicking nothing in Replace means nothing is selected, which is
        // what every other selection tool on earth does.
        if (_last.combine == Combine::Replace && !_doc->sel().empty())
            select_all(false);
        return;
    }
    select_seed(hit);
}

void EditSession::keep_largest_components() {
    if (!_doc) return;
    _last = LastAction{};
    _last.kind = LastAction::Floaters;
    _last.layer = _doc->layer();
    _last.before = _doc->sel().weights();
    _last.combine = Combine::Replace;
    run_last(/*fresh=*/true);
}

void EditSession::select_all(bool on) {
    if (!_doc) return;
    std::vector<uint8_t> w((size_t)_doc->count(), 0);
    if (on)
        for (int64_t i = 0; i < _doc->count(); i++)
            if (_doc->alive()[i]) w[(size_t)i] = 255;
    run_select(std::move(w), on ? msg::op_select_all : msg::op_select_none);
}

void EditSession::invert_selection() {
    if (!_doc) return;
    std::vector<uint8_t> w = _doc->sel().weights();
    for (int64_t i = 0; i < _doc->count(); i++)
        w[(size_t)i] = _doc->alive()[i] ? (uint8_t)(255 - w[(size_t)i]) : 0;
    run_select(std::move(w), msg::op_select_invert);
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


// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

bool EditSession::can_save_in_place() const {
    return _doc && !_doc->default_save_path(_save_target).empty();
}

bool EditSession::can_save_copy() const {
    if (!_doc || !_pick_save) return false;
    const std::vector<SaveTarget> t = _doc->save_targets();
    return _save_target >= 0 && _save_target < (int)t.size() &&
           !t[(size_t)_save_target].folder;
}

void EditSession::save_in_place() {
    if (!_doc) return;
    save_to(_save_target, _doc->default_save_path(_save_target));
}

void EditSession::ask_save_copy() {
    if (!_doc || !_pick_save) return;
    const std::vector<SaveTarget> t = _doc->save_targets();
    if (_save_target < 0 || _save_target >= (int)t.size()) return;
    const SaveTarget& target = t[(size_t)_save_target];
    if (target.folder) return;
    const std::string home = _doc->default_save_path(_save_target);
    std::string stem =
        std::filesystem::path(home.empty() ? _doc->source_path() : home)
            .stem()
            .string();
    if (stem.empty()) stem = "model";
    _pick_save(_save_target, target.ext, target.folder,
               stem + "_edited" + target.ext);
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
