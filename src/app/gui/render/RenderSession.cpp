// RenderSession.cpp -- the render mode's state, its viewport interaction and
// the export; the panel is RenderPanel.cpp, the timeline RenderTimeline.cpp.

#include "app/gui/render/RenderSession.h"

#include "app/AppPaths.h"
#include "app/gui/Layout.h"
#include "app/gui/Subprocess.h"
#include "app/gui/ViewportPanel.h"
#include "app/gui/edit/SelectShape.h"
#include "i18n/catalog/Render.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
namespace msg = spirula::i18n::msg::render;
using spirula::Sim3;
using spirula::i18n::format;

namespace gui::render {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr ImU32 kPathCol = IM_COL32(255, 214, 102, 220);
constexpr ImU32 kKeyCol = IM_COL32(235, 235, 235, 230);
constexpr ImU32 kSelCol = IM_COL32(255, 150, 40, 255);
constexpr ImU32 kHotCol = IM_COL32(255, 235, 90, 255);
constexpr ImU32 kHeadCol = IM_COL32(90, 220, 255, 255);
constexpr ImU32 kAxis[3] = {IM_COL32(250, 51, 79, 255), IM_COL32(140, 219, 0, 255),
                            IM_COL32(41, 140, 250, 255)};

double now_s() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void rot_of_c2w(const float c2w[12], double R[9]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) R[r*3+c] = c2w[r*4+c];
}

// A rotation (w, x, y, z) taken through a similarity's rotation.
void rotate_quat(const Sim3& s, const double q[4], double out[4]) {
    double R[9], M[9];
    quat_to_matrix3(q, R);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            double v = 0.0;
            for (int k = 0; k < 3; k++) v += s.R[r*3+k] * R[k*3+c];
            M[r*3+c] = v;
        }
    quat_from_matrix3(M, out);
}

double smooth(double u) {
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3.0 - 2.0 * u);
}

}  // namespace


RenderSession::RenderSession() = default;

RenderSession::~RenderSession() {
    cancel_export();
    if (_probe.joinable()) _probe.join();
}

void RenderSession::note(const std::string& s) {
    _log.push_back(s);
    _status = s;
    _status_err = false;
}

std::vector<std::string> RenderSession::drain_log() {
    std::vector<std::string> out;
    out.swap(_log);
    return out;
}

void RenderSession::open(ViewportPanel* panel) {
    _panel = panel;
    if (_panel) _panel->set_interactor(this);
    _preview_key.clear();
    if (_encoder_probe.load() == 0) probe_encoder();
}

void RenderSession::close(bool switching) {
    cancel_export();
    // A camera move is work: one left unsaved goes to autosave.json, never
    // over a project the user named.
    if (_panel && _have_project && _project.keys.size() >= 2 && dirty() && !_sources.empty()) {
        const std::string dir = default_project_dir(_sources[0].path);
        if (!dir.empty()) {
            try {
                const std::string path = (fs::u8path(dir) / "autosave.json").string();
                _project.placement = primary_placement();
                save_project(_project, path);
                if (!switching) _log.push_back(format(msg::project_autosaved, {path}));
            } catch (const std::exception&) {
            }
        }
    }
    _playing = false;
    _xform.cancel();
    _pick_target = _pick_waiting = false;
    if (_panel) _panel->set_interactor(nullptr);
    _panel = nullptr;
    _frames.set_sources({});
    _frames.destroy_gl();
    _preview_key.clear();
}

bool RenderSession::animating() const {
    return _playing || _job.state != Job::Idle || _frames.busy() || _pick_waiting;
}

bool RenderSession::dirty() const {
    return _have_project && !_project.keys.empty() &&
           project_to_json(_project) != _saved_json;
}


// ===========================================================================
// Sources
// ===========================================================================

void RenderSession::set_sources(std::vector<SourceInfo> sources) {
    _sources = std::move(sources);
    // A dataset read after the project started still decides up, as long
    // as nothing has been laid out against the old one.
    if (_have_project && !_up_known && _project.keys.size() <= 1)
        for (const SourceInfo& s : _sources)
            if (s.has_up) {
                const Sim3 to_world = s.view.norm_to_world * s.view.file_to_norm;
                to_world.rotate(s.up, _project.up);
                _up_known = true;
                break;
            }
    std::vector<SourceView> views;
    views.reserve(_sources.size());
    for (const SourceInfo& s : _sources) views.push_back(s.view);
    _frames.set_sources(views);
    if (!_have_project) return;
    // The project's list follows what is open, by position; a style set for
    // a model that has gone stays for the one that takes its place.
    if (_project.sources.size() < _sources.size())
        _project.sources.resize(_sources.size());
    for (size_t i = 0; i < _sources.size(); i++)
        _project.sources[i].path = _sources[i].path;
}


// ===========================================================================
// Project and history
// ===========================================================================

void RenderSession::new_project() {
    _project = RenderProject();
    _have_project = true;
    for (const SourceInfo& s : _sources) {
        Source src;
        src.path = s.path;
        if (s.view.kind == SourceView::Points) src.style.point_style = PointStyle::Circle;
        _project.sources.push_back(src);
    }
    // Up is the dataset's when one says, else whatever the pane shows as up.
    const Sim3 s2w = _w2s.inverse();
    const double z[3] = {0, 0, 1};
    s2w.rotate(z, _project.up);
    _up_known = false;
    for (const SourceInfo& s : _sources)
        if (s.has_up) {
            (s.view.norm_to_world * s.view.file_to_norm).rotate(s.up, _project.up);
            _up_known = true;
            break;
        }
    if (_panel) {
        add_key_from_view(0.0, true);
        if (_panel->view_model() == 3) {
            _project.output.width = 3840;
            _project.output.height = 1920;
        }
    }
    _project_path.clear();
    _saved_json = project_to_json(_project);
    _stable_json = _saved_json;
    _undo.clear();
    _redo.clear();
    _time = 0.0;
    project_changed();
}

void RenderSession::project_changed() {
    _revision++;
    _sel.resize(_project.keys.size(), 0);
}

const Trajectory& RenderSession::trajectory() {
    if (!_traj || _traj_rev != _revision) {
        _traj = std::make_unique<Trajectory>(_project);
        _traj_rev = _revision;
    }
    return *_traj;
}

void RenderSession::commit_history() {
    if (!_have_project) return;
    const std::string cur = project_to_json(_project);
    if (cur == _stable_json) return;
    // Mid-gesture: a drag is one step, taken when it lets go.
    if (ImGui::IsAnyItemActive() || _xform.active() || _drag_key >= 0 || _scrubbing)
        return;
    if (!_stable_json.empty()) {
        _undo.push_back(_stable_json);
        if (_undo.size() > 200) _undo.erase(_undo.begin());
        _redo.clear();
    }
    _stable_json = cur;
    project_changed();
}

void RenderSession::undo() {
    if (_undo.empty()) return;
    _redo.push_back(project_to_json(_project));
    try {
        _project = project_from_json(_undo.back());
    } catch (const std::exception&) {
    }
    _undo.pop_back();
    _stable_json = project_to_json(_project);
    // Same keys, same selection: undoing a move leaves the camera in hand.
    if (_sel.size() != _project.keys.size()) _sel.assign(_project.keys.size(), 0);
    project_changed();
}

void RenderSession::redo() {
    if (_redo.empty()) return;
    _undo.push_back(project_to_json(_project));
    try {
        _project = project_from_json(_redo.back());
    } catch (const std::exception&) {
    }
    _redo.pop_back();
    _stable_json = project_to_json(_project);
    if (_sel.size() != _project.keys.size()) _sel.assign(_project.keys.size(), 0);
    project_changed();
}

// The primary model's placement now, file coordinates: what the poses are
// laid out against.
Sim3 RenderSession::primary_placement() const {
    if (_sources.empty()) return Sim3();
    return _sources[0].view.norm_to_world * _sources[0].view.file_to_norm;
}

void RenderSession::save_to(const std::string& path) {
    _project.placement = primary_placement();
    try {
        save_project(_project, path);
        _project_path = path;
        _saved_json = project_to_json(_project);
        note(format(msg::project_saved, {path}));
    } catch (const std::exception& e) {
        _status = format(msg::project_save_failed, {e.what()});
        _status_err = true;
        _log.push_back(_status);
    }
}

void RenderSession::open_from(const std::string& path) {
    try {
        RenderProject p = load_project(path);
        // Laid out against another placement of this model: carried across.
        const Sim3 now = primary_placement();
        Sim3 diff = now * p.placement.inverse();
        if (!diff.is_identity(1e-9)) transform_project(p, diff);
        p.placement = now;
        _project = std::move(p);
        _have_project = true;
        // The models are whatever is open now, in the order it is open.
        if (_project.sources.size() < _sources.size())
            _project.sources.resize(_sources.size());
        for (size_t i = 0; i < _sources.size(); i++)
            _project.sources[i].path = _sources[i].path;
        _project_path = path;
        _saved_json = project_to_json(_project);
        _stable_json = _saved_json;
        _undo.clear();
        _redo.clear();
        _sel.assign(_project.keys.size(), 0);
        _time = 0.0;
        project_changed();
        note(format(msg::project_opened, {path}));
    } catch (const std::exception& e) {
        _status = format(msg::project_open_failed, {e.what()});
        _status_err = true;
        _log.push_back(_status);
    }
}

void RenderSession::picked(Pick kind, const std::string& path) {
    if (path.empty()) return;
    switch (kind) {
        case Pick::SaveProject: save_to(path); break;
        case Pick::OpenProject: open_from(path); break;
        case Pick::Output:
            _project.output.path = path;
            project_changed();
            break;
        case Pick::AddModel: break;
    }
}


// ===========================================================================
// Keyframes
// ===========================================================================

bool RenderSession::view_pose(double pos[3], double rot[4], double target[3]) const {
    if (!_panel) return false;
    float c2w[12], tgt[3];
    _panel->nav_pose(c2w, tgt);
    const Sim3 s2w = _w2s.inverse();
    const double p[3] = {c2w[3], c2w[7], c2w[11]}, t[3] = {tgt[0], tgt[1], tgt[2]};
    s2w.apply(p, pos);
    s2w.apply(t, target);
    double R[9], q[4];
    rot_of_c2w(c2w, R);
    quat_from_matrix3(R, q);
    rotate_quat(s2w, q, rot);
    return true;
}

bool RenderSession::selected(int i) const {
    return i >= 0 && i < (int)_sel.size() && _sel[(size_t)i];
}

int RenderSession::single_selected() const {
    int found = -1;
    for (int i = 0; i < (int)_sel.size(); i++)
        if (_sel[(size_t)i]) {
            if (found >= 0) return -1;
            found = i;
        }
    return found;
}

void RenderSession::select_only(int index) {
    _sel.assign(_project.keys.size(), 0);
    if (index >= 0 && index < (int)_sel.size()) _sel[(size_t)index] = 1;
}

int RenderSession::add_key_from_view(double time, bool select) {
    Keyframe k;
    k.time = std::max(0.0, time);
    if (!view_pose(k.pos, k.rot, k.target)) return -1;
    if (_project.keys.empty()) {
        k.own_lens = true;
        k.lens.projection = (Projection)std::clamp(_panel->view_model(), 0, 3);
        lens_set_fov(k.lens, _panel->view_fov());
    }
    _project.keys.push_back(k);
    const double t = k.time;
    _project.sort_keys();
    int index = 0;
    for (int i = 0; i < (int)_project.keys.size(); i++)
        if (_project.keys[(size_t)i].time == t) index = i;
    _sel.insert(_sel.begin() + index, 0);
    project_changed();
    if (select) select_only(index);
    return index;
}

void RenderSession::update_key_from_view(int index) {
    if (index < 0 || index >= (int)_project.keys.size()) return;
    Keyframe& k = _project.keys[(size_t)index];
    double target[3];
    if (!view_pose(k.pos, k.rot, target)) return;
    if (k.aim) k.roll = roll_of(k.rot, k.pos, k.target, _project.up);
    update_aim(k, _project.up);
    project_changed();
}

void RenderSession::look_through(double time) {
    if (!_panel || _project.keys.empty()) return;
    const CameraState c = trajectory().at(time);
    double c2w[12];
    c.c2w(c2w);
    const double p[3] = {c2w[3], c2w[7], c2w[11]};
    double ps[3];
    _w2s.apply(p, ps);
    double R[9], Rs[9];
    quat_to_matrix3(c.rot, R);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            double v = 0.0;
            for (int m = 0; m < 3; m++) v += _w2s.R[r*3+m] * R[m*3+k];
            Rs[r*3+k] = v;
        }
    float out[12];
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) out[r*4+k] = (float)Rs[r*3+k];
        out[r*4+3] = (float)ps[r];
    }
    // The pivot stays as far ahead as it was, so orbiting from here turns
    // about something in front of the camera.
    float nav[12], tgt[3];
    _panel->nav_pose(nav, tgt);
    float d = std::sqrt((tgt[0]-nav[3])*(tgt[0]-nav[3]) + (tgt[1]-nav[7])*(tgt[1]-nav[7]) +
                        (tgt[2]-nav[11])*(tgt[2]-nav[11]));
    if (!(d > 1e-6f)) d = 1.0f;
    const float ahead[3] = {out[3] - out[2] * d, out[7] - out[6] * d, out[11] - out[10] * d};
    _panel->set_nav_pose(out, ahead);
    _panel->set_view_lens((int)c.lens.projection, (float)lens_fov(c.lens));
}

void RenderSession::delete_selected() {
    std::vector<Keyframe> kept;
    for (size_t i = 0; i < _project.keys.size(); i++)
        if (!selected((int)i)) kept.push_back(_project.keys[i]);
    if (kept.size() == _project.keys.size()) return;
    _project.keys = std::move(kept);
    _project.sort_keys();
    _sel.assign(_project.keys.size(), 0);
    project_changed();
}

void RenderSession::space_evenly(double total) {
    const int n = (int)_project.keys.size();
    if (n < 2) return;
    total = std::max(total, 0.1 * (n - 1));
    for (int i = 0; i < n; i++) _project.keys[(size_t)i].time = total * i / (n - 1);
    project_changed();
}

// The view's own up, for a model no dataset levels: turn the view until the
// horizon is level, then take it.
void RenderSession::take_up_from_view() {
    double pos[3], rot[4], target[3];
    if (!view_pose(pos, rot, target)) return;
    double R[9];
    quat_to_matrix3(rot, R);
    for (int k = 0; k < 3; k++) _project.up[k] = R[k*3+1];
    _up_known = true;
    for (Keyframe& k : _project.keys) update_aim(k, _project.up);
    project_changed();
}

// A full turn about the point the view orbits, at the view's own distance
// and height: the classic product shot, as ordinary keys.
void RenderSession::make_orbit() {
    if (!_panel) return;
    double pos[3], rot[4], target[3];
    if (!view_pose(pos, rot, target)) return;
    const double* up = _project.up;
    double rel[3] = {pos[0] - target[0], pos[1] - target[1], pos[2] - target[2]};
    double h = rel[0]*up[0] + rel[1]*up[1] + rel[2]*up[2];
    double flat[3] = {rel[0] - h*up[0], rel[1] - h*up[1], rel[2] - h*up[2]};
    double r = std::sqrt(flat[0]*flat[0] + flat[1]*flat[1] + flat[2]*flat[2]);
    const double dist = std::sqrt(r*r + h*h);
    if (!(dist > 1e-9)) return;
    // From overhead the circle would shrink to a spin on the spot: tip it to
    // 30 degrees, toward the bottom of the picture so the scene keeps its way up.
    if (r < 0.5 * dist) {
        if (r < 1e-3 * dist) {
            const double y[3] = {0, 1, 0};
            double sy[3];
            quat_rotate(rot, y, sy);
            const double k = sy[0]*up[0] + sy[1]*up[1] + sy[2]*up[2];
            for (int d = 0; d < 3; d++) flat[d] = -(sy[d] - k * up[d]);
            r = std::sqrt(flat[0]*flat[0] + flat[1]*flat[1] + flat[2]*flat[2]);
            if (!(r > 1e-9)) return;
        }
        for (double& v : flat) v *= dist * std::cos(kPi / 6) / r;
        r = dist * std::cos(kPi / 6);
        h = (h < 0 ? -1.0 : 1.0) * dist * std::sin(kPi / 6);
    }
    double side[3] = {up[1]*flat[2] - up[2]*flat[1], up[2]*flat[0] - up[0]*flat[2],
                      up[0]*flat[1] - up[1]*flat[0]};
    const double sl = std::sqrt(side[0]*side[0] + side[1]*side[1] + side[2]*side[2]);
    for (double& v : side) v /= std::max(sl, 1e-12);
    const Lens lens = _project.keys.empty() ? Lens() : _project.lens_at(0);
    _project.keys.clear();
    constexpr int kSteps = 8;
    constexpr double kSeconds = 12.0;
    for (int i = 0; i <= kSteps; i++) {
        const double a = 2.0 * kPi * i / kSteps;
        Keyframe k;
        k.time = kSeconds * i / kSteps;
        for (int d = 0; d < 3; d++)
            k.pos[d] = target[d] + h * up[d] + std::cos(a) * flat[d] + std::sin(a) * r * side[d];
        k.aim = true;
        for (int d = 0; d < 3; d++) k.target[d] = target[d];
        if (i == 0) { k.own_lens = true; k.lens = lens; }
        update_aim(k, _project.up);
        _project.keys.push_back(k);
    }
    _project.motion.smooth = true;
    _project.motion.constant_speed = true;
    _project.motion.ease = false;
    _project.sort_keys();
    _sel.assign(_project.keys.size(), 0);
    _time = 0.0;
    project_changed();
}

// The capture's own path: the dataset's cameras in the order they were
// taken, thinned to a key every few metres of travel.
void RenderSession::follow_capture() {
    const ParsedDataset* ds = nullptr;
    int source = -1;
    for (size_t i = 0; i < _sources.size() && !ds; i++) {
        if (_sources[i].view.kind == SourceView::Points) { ds = _sources[i].view.ds; source = (int)i; }
        else if (_sources[i].dataset) { ds = _sources[i].dataset; source = (int)i; }
    }
    if (!ds || ds->num_cameras < 2) return;
    // Cameras live in the model's file frame: the points' own, or the
    // training frame a model from the dataset was saved in.
    const Sim3 to_world = _sources[(size_t)source].view.norm_to_world *
                          _sources[(size_t)source].view.file_to_norm;
    std::vector<int> order((size_t)ds->num_cameras);
    for (int i = 0; i < (int)order.size(); i++) order[(size_t)i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return ds->image_filenames.size() > (size_t)std::max(a, b)
                   ? ds->image_filenames[(size_t)a] < ds->image_filenames[(size_t)b]
                   : a < b;
    });
    const int want = std::clamp((int)order.size() / 12, 4, 16);
    // The capture's own lens, as the frames were taken.
    Lens lens = _project.keys.empty() ? Lens() : _project.lens_at(0);
    const std::vector<DatasetLens> lenses = cluster_dataset_lenses(*ds);
    if (!lenses.empty()) lens = lenses.front().lens;
    _project.keys.clear();
    for (int j = 0; j < want; j++) {
        const int i = order[(size_t)((order.size() - 1) * j / (want - 1))];
        const float* M = &ds->c2w[(size_t)i * 12];
        Keyframe k;
        k.time = 1.5 * j;
        const double p[3] = {M[3], M[7], M[11]};
        to_world.apply(p, k.pos);
        double R[9], q[4];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) R[r*3+c] = M[r*4+c];
        quat_from_matrix3(R, q);
        rotate_quat(to_world, q, k.rot);
        if (j == 0) { k.own_lens = true; k.lens = lens; }
        _project.keys.push_back(k);
    }
    _project.motion.smooth = true;
    _project.motion.ease = true;
    _project.sort_keys();
    _sel.assign(_project.keys.size(), 0);
    _time = 0.0;
    project_changed();
}


// ===========================================================================
// Viewport
// ===========================================================================

bool RenderSession::view_projection(ViewProjection& vp, int W, int H) const {
    if (!_panel || W <= 0 || H <= 0) return false;
    _panel->nav_camera(W, H, vp.w2c, vp.fx, vp.fy, vp.camera_model, vp.eye);
    vp.cx = 0.5f * W;
    vp.cy = 0.5f * H;
    vp.W = W;
    vp.H = H;
    vp.ortho_back = _panel->ortho_pullback(true);
    return true;
}

void RenderSession::to_shared(const double w[3], double s[3]) const { _w2s.apply(w, s); }

bool RenderSession::key_screen(int index, const ViewProjection& vp, float& x,
                               float& y) const {
    double s[3];
    to_shared(_project.keys[(size_t)index].pos, s);
    const float p[3] = {(float)s[0], (float)s[1], (float)s[2]};
    float d;
    return vp.project(p, x, y, d) && d > 0.0f;
}

int RenderSession::hit_key(float x, float y) const {
    float ix, iy, iw, ih;
    if (!_panel) return -1;
    _panel->image_rect(ix, iy, iw, ih);
    ViewProjection vp;
    if (!view_projection(vp, (int)iw, (int)ih)) return -1;
    int best = -1;
    float best_d = px(12.0f);
    for (int i = 0; i < (int)_project.keys.size(); i++) {
        float kx, ky;
        if (!key_screen(i, vp, kx, ky)) continue;
        const float d = std::hypot(kx - x, ky - y);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

bool RenderSession::xform_frame(XformFrame& f, XformKind kind) {
    float ix, iy, iw, ih;
    if (!_panel) return false;
    _panel->image_rect(ix, iy, iw, ih);
    if (!view_projection(f.cam, (int)iw, (int)ih)) return false;
    const std::vector<Keyframe>& keys = _xform.active() ? _xform_from : _project.keys;
    double c[3] = {0, 0, 0};
    int n = 0, one = -1;
    for (int i = 0; i < (int)keys.size() && i < (int)_sel.size(); i++) {
        if (!_sel[(size_t)i]) continue;
        for (int d = 0; d < 3; d++) c[d] += keys[(size_t)i].pos[d];
        n++;
        one = i;
    }
    if (!n) return false;
    for (double& v : c) v /= n;
    // One aimed camera turns about what it looks at: R orbits it.
    if (n == 1 && keys[(size_t)one].aim && kind == XformKind::Rotate)
        for (int d = 0; d < 3; d++) c[d] = keys[(size_t)one].target[d];
    to_shared(c, f.pivot);
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) f.global_axes[a*3+k] = _w2s.R[k*3+a];
    if (n == 1) {
        double R[9];
        quat_to_matrix3(keys[(size_t)one].rot, R);
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 3; k++) {
                double v = 0.0;
                for (int m = 0; m < 3; m++) v += _w2s.R[k*3+m] * R[m*3+a];
                f.local_axes[a*3+k] = v;
            }
    } else {
        for (int i = 0; i < 9; i++) f.local_axes[i] = f.global_axes[i];
    }
    f.unit = _w2s.s;
    f.grid_cell = _panel->world_grid_cell();
    return true;
}

void RenderSession::begin_xform(XformKind kind, float mx, float my, bool drag,
                                int axis, bool plane) {
    if (_project.keys.empty()) return;
    bool any = false;
    for (uint8_t s : _sel) any = any || s;
    if (!any) return;
    _xform_from = _project.keys;
    XformFrame f;
    if (!xform_frame(f, kind)) return;
    _xform.begin(kind, f, mx, my, drag, axis, plane);
}

void RenderSession::apply_xform(const Sim3& step_shared, bool scale_fov, double factor) {
    const Sim3 s2w = _w2s.inverse();
    const Sim3 step = s2w * step_shared * _w2s;
    int n = 0;
    for (uint8_t s : _sel) n += s ? 1 : 0;
    _project.keys = _xform_from;
    for (int i = 0; i < (int)_project.keys.size() && i < (int)_sel.size(); i++) {
        if (!_sel[(size_t)i]) continue;
        Keyframe& k = _project.keys[(size_t)i];
        if (scale_fov) {
            if (!k.own_lens) { k.lens = _project.lens_at(i); k.own_lens = true; }
            // A bigger frustum is a wider view.
            const double fov = lens_fov(k.lens);
            const double t = std::tan(std::min(fov, 179.0) * kPi / 360.0) * factor;
            if (k.lens.projection == Projection::Perspective)
                lens_set_fov(k.lens, std::clamp(std::atan(t) * 360.0 / kPi, 1.0, 170.0));
            else if (k.lens.projection != Projection::Equirect)
                lens_set_fov(k.lens, std::clamp(fov * factor, 10.0, 360.0));
            continue;
        }
        const bool orbit_one = n == 1 && k.aim;
        double p[3];
        step.apply(k.pos, p);
        const bool pure_turn = _xform.kind() == XformKind::Rotate && n == 1 && !k.aim;
        if (!pure_turn) for (int d = 0; d < 3; d++) k.pos[d] = p[d];
        if (k.aim) {
            if (!orbit_one) {
                double t[3];
                step.apply(k.target, t);
                for (int d = 0; d < 3; d++) k.target[d] = t[d];
            }
            update_aim(k, _project.up);
        } else if (_xform.kind() == XformKind::Rotate) {
            double q[4];
            rotate_quat(step, k.rot, q);
            for (int d = 0; d < 4; d++) k.rot[d] = q[d];
        }
    }
    project_changed();
}

bool RenderSession::owns_left_button() const {
    return _xform.active() || _pick_target || _pick_waiting || _hot >= 0 ||
           _handle_hot >= 0;
}

bool RenderSession::blocks_fly_keys() const {
    if (_xform.active() || _fly_block_key != 0) return true;
    for (uint8_t s : _sel)
        if (s) return true;
    return false;
}

bool RenderSession::on_viewport_input(const ViewportInput& in) {
    _mouse[0] = in.x;
    _mouse[1] = in.y;
    _mouse_in = in.hovered;
    if (_project.keys.empty() && !_pick_target) { _hot = _handle_hot = -1; return false; }

    if (_xform.active()) {
        XformFrame f;
        if (!xform_frame(f, _xform.kind())) { _xform.cancel(); return true; }
        const TransformTool::Result r = _xform.update(in, f);
        const bool fov = _xform.kind() == XformKind::Scale;
        if (r == TransformTool::Result::Cancelled) {
            _project.keys = _xform_from;
            project_changed();
        } else {
            apply_xform(_xform.delta(), fov, _xform.delta().s);
        }
        return true;
    }
    if (_pick_target) {
        if (in.clicked) {
            _panel->request_pick(in.x / std::max(in.W, 1), in.y / std::max(in.H, 1));
            _pick_waiting = true;
            _pick_target = false;
        } else if (in.right_clicked || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            _pick_target = false;
        }
        return true;
    }

    _handle_hot = -1;
    const int one = single_selected();
    if (one >= 0) {
        XformFrame f;
        if (xform_frame(f, _handle_mode)) {
            _handle_hot = _xform.hit_handle(_handle_mode, f, in.x, in.y);
            if (_handle_hot >= 0 && in.clicked) {
                const int axis = _handle_hot < 3 ? _handle_hot
                               : _handle_hot < 6 ? _handle_hot - 3 : -1;
                begin_xform(_handle_mode, in.x, in.y, true, axis, _handle_hot >= 3 && _handle_hot < 6);
                return true;
            }
        }
    }
    _hot = in.hovered ? hit_key(in.x, in.y) : -1;
    if (_hot >= 0 && in.clicked) {
        if (in.shift || in.ctrl) {
            if (_hot < (int)_sel.size()) _sel[(size_t)_hot] ^= 1;
        } else if (!selected(_hot)) {
            select_only(_hot);
        }
        _time = _project.keys[(size_t)_hot].time;
        // Pressing on a camera and dragging moves it, the way a handle does.
        if (selected(_hot)) begin_xform(XformKind::Move, in.x, in.y, true);
        return true;
    }
    return _hot >= 0 || _handle_hot >= 0;
}

void RenderSession::draw_camera(ImDrawList* dl, const ViewProjection& vp, float ox,
                                float oy, const CameraState& c, unsigned col,
                                float size, bool axes) const {
    double s[3];
    to_shared(c.pos, s);
    const float sp[3] = {(float)s[0], (float)s[1], (float)s[2]};
    float cx, cy, depth;
    if (!vp.project(sp, cx, cy, depth) || depth <= 0.0f) return;
    // A fixed size on screen: the cameras stay readable whatever the zoom.
    double R[9];
    quat_to_matrix3(c.rot, R);
    const double right_s[3] = {_w2s.R[0]*R[0] + _w2s.R[1]*R[3] + _w2s.R[2]*R[6],
                               _w2s.R[3]*R[0] + _w2s.R[4]*R[3] + _w2s.R[5]*R[6],
                               _w2s.R[6]*R[0] + _w2s.R[7]*R[3] + _w2s.R[8]*R[6]};
    const float h = std::max(depth * 1e-3f, 1e-7f);
    const float q[3] = {sp[0] + (float)right_s[0] * h, sp[1] + (float)right_s[1] * h,
                        sp[2] + (float)right_s[2] * h};
    float qx, qy, qd;
    double pps = 0.0;
    if (vp.project(q, qx, qy, qd)) pps = std::hypot(qx - cx, qy - cy) / h;
    if (!(pps > 1e-9)) return;
    const double L = size / pps / _w2s.s;   // world units
    // Frustum corners at depth L, in the camera's own axes (x right, y up,
    // -z ahead); a wide lens is drawn at a fov a pyramid can still show.
    const double fov = std::min(lens_fov(c.lens), 150.0) * kPi / 180.0;
    const double aspect = (double)_project.output.width / std::max(_project.output.height, 1);
    const double tx = std::tan(fov * 0.5), ty = tx / aspect;
    auto world_of = [&](double x, double y, double z, double out[3]) {
        for (int r = 0; r < 3; r++)
            out[r] = c.pos[r] + L * (R[r*3+0]*x + R[r*3+1]*y + R[r*3+2]*z);
    };
    auto screen = [&](const double w[3], ImVec2& out) {
        double sh[3];
        to_shared(w, sh);
        const float p[3] = {(float)sh[0], (float)sh[1], (float)sh[2]};
        float x, y, d;
        if (!vp.project(p, x, y, d) || d <= 0.0f) return false;
        out = ImVec2(ox + x, oy + y);
        return true;
    };
    const ImVec2 apex(ox + cx, oy + cy);
    const float th = px(1.5f);
    // A 360 camera looks everywhere: a horizon ring and an arrow where the
    // middle of the picture is, not a pyramid.
    if (c.lens.projection == Projection::Equirect) {
        ImVec2 last;
        for (int s = 0; s <= 32; s++) {
            const double a = 2.0 * kPi * s / 32.0;
            double w[3];
            world_of(0.6 * std::sin(a), 0.0, -0.6 * std::cos(a), w);
            ImVec2 e;
            if (!screen(w, e)) continue;
            if (s) dl->AddLine(last, e, col, th);
            last = e;
        }
        double tip[3];
        world_of(0.0, 0.0, -1.0, tip);
        ImVec2 t;
        if (screen(tip, t)) dl->AddLine(apex, t, col, px(2.0f));
    }
    const double corners[4][2] = {{-tx, ty}, {tx, ty}, {tx, -ty}, {-tx, -ty}};
    ImVec2 cs[4];
    bool ok = c.lens.projection != Projection::Equirect;
    for (int i = 0; i < 4 && ok; i++) {
        double w[3];
        world_of(corners[i][0], corners[i][1], -1.0, w);
        ok = screen(w, cs[i]);
    }
    if (ok) {
        for (int i = 0; i < 4; i++) {
            dl->AddLine(apex, cs[i], col, th);
            dl->AddLine(cs[i], cs[(i + 1) % 4], col, th);
        }
        // Which way is up: the triangle on the top edge every camera gizmo has.
        double tip[3];
        world_of(0.0, ty * 1.6, -1.0, tip);
        ImVec2 t;
        if (screen(tip, t)) dl->AddTriangleFilled(cs[0], cs[1], t, (col & 0x00ffffff) | 0x90000000);
    }
    if (axes) {
        for (int a = 0; a < 3; a++) {
            double w[3];
            world_of(a == 0 ? 0.7 : 0.0, a == 1 ? 0.7 : 0.0, a == 2 ? 0.7 : 0.0, w);
            ImVec2 e;
            if (!screen(w, e)) continue;
            dl->AddLine(apex, e, kAxis[a], px(2.0f));
            const float dx = e.x - apex.x, dy = e.y - apex.y;
            const float l = std::max(std::hypot(dx, dy), 1e-3f);
            const float ux = dx / l, uy = dy / l, hh = px(7.0f), ww = px(3.5f);
            dl->AddTriangleFilled(ImVec2(e.x + ux * hh, e.y + uy * hh),
                                  ImVec2(e.x - uy * ww, e.y + ux * ww),
                                  ImVec2(e.x + uy * ww, e.y - ux * ww), kAxis[a]);
        }
    }
    dl->AddCircleFilled(apex, px(3.5f), col, 12);
}

void RenderSession::draw_viewport_overlay(const ViewportOverlay& v) {
    if (!_have_project || !_panel) return;
    ImDrawList* dl = v.dl;
    ViewProjection vp;
    if (!view_projection(vp, (int)v.w, (int)v.h)) return;
    const Trajectory& tr = trajectory();

    // The path.
    if (_project.keys.size() >= 2) {
        const int n = std::clamp((int)_project.keys.size() * 40, 80, 800);
        const std::vector<double> pts = tr.sample_path(n);
        ImVec2 last;
        bool have = false;
        for (size_t i = 0; i + 2 < pts.size(); i += 3) {
            double s[3];
            to_shared(&pts[i], s);
            const float p[3] = {(float)s[0], (float)s[1], (float)s[2]};
            float x, y, d;
            if (!vp.project(p, x, y, d) || d <= 0.0f) { have = false; continue; }
            const ImVec2 e(v.x + x, v.y + y);
            if (have) dl->AddLine(last, e, kPathCol, px(2.0f));
            last = e;
            have = true;
        }
    }

    // Aimed keys: the point each looks at.
    for (size_t i = 0; i < _project.keys.size(); i++) {
        const Keyframe& k = _project.keys[i];
        if (!k.aim) continue;
        double s[3], c[3];
        to_shared(k.target, s);
        to_shared(k.pos, c);
        const float p[3] = {(float)s[0], (float)s[1], (float)s[2]};
        const float q[3] = {(float)c[0], (float)c[1], (float)c[2]};
        float x, y, d, x2, y2, d2;
        if (!vp.project(p, x, y, d) || d <= 0.0f) continue;
        const ImVec2 t(v.x + x, v.y + y);
        const ImU32 col = selected((int)i) ? kSelCol : IM_COL32(255, 255, 255, 140);
        dl->AddCircle(t, px(6.0f), col, 16, px(1.5f));
        dl->AddLine(ImVec2(t.x - px(9), t.y), ImVec2(t.x + px(9), t.y), col, px(1.0f));
        dl->AddLine(ImVec2(t.x, t.y - px(9)), ImVec2(t.x, t.y + px(9)), col, px(1.0f));
        if (selected((int)i) && vp.project(q, x2, y2, d2) && d2 > 0.0f)
            dl->AddLine(t, ImVec2(v.x + x2, v.y + y2), (col & 0x00ffffff) | 0x60000000, px(1.0f));
    }

    // The keys, then the camera at the playhead on top of them.
    for (int i = 0; i < (int)_project.keys.size(); i++) {
        const Keyframe& k = _project.keys[(size_t)i];
        CameraState c;
        for (int d = 0; d < 3; d++) c.pos[d] = k.pos[d];
        for (int d = 0; d < 4; d++) c.rot[d] = k.rot[d];
        c.lens = k.own_lens ? k.lens : _project.lens_at(i);
        const ImU32 col = selected(i) ? kSelCol : i == _hot ? kHotCol : kKeyCol;
        draw_camera(dl, vp, v.x, v.y, c, col, px(selected(i) ? 34.0f : 26.0f),
                    !selected(i) || _project.keys.size() > 1);
    }
    if (!_project.keys.empty())
        draw_camera(dl, vp, v.x, v.y, tr.at(_time), kHeadCol, px(40.0f), false);

    // The handles of the one selected key, or the operator running on it.
    XformFrame f;
    if (xform_frame(f, _xform.active() ? _xform.kind() : _handle_mode)) {
        const ImVec2 origin(v.x, v.y);
        if (_xform.active()) _xform.draw_overlay(dl, origin, f);
        else if (single_selected() >= 0) _xform.draw_handles(dl, origin, _handle_mode, f, _handle_hot);
    }

    // The camera's picture in the corner -- the other corner when the
    // selected camera is under it.
    if (_preview_mode == PreviewMode::Corner && _frames.texture() && _frames.width() > 0) {
        const float w = std::min(v.w * 0.32f, px(420.0f));
        const float h = w * (float)_frames.height() / (float)_frames.width();
        preview_size(w, h, _preview_w, _preview_h);
        const float m = px(10.0f);
        ImVec2 a(v.x + v.w - w - m, v.y + v.h - h - m);
        const int one = single_selected();
        float kx, ky;
        if (one >= 0 && key_screen(one, vp, kx, ky) && v.x + kx > a.x - px(40.0f) &&
            v.y + ky > a.y - px(40.0f))
            a.x = v.x + m;
        const ImVec2 b(a.x + w, a.y + h);
        dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2),
                          IM_COL32(0, 0, 0, 200));
        dl->AddImage((ImTextureID)(intptr_t)_frames.texture(), a, b, ImVec2(0, 1), ImVec2(1, 0));
        dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), kHeadCol, 0.0f, 0,
                    px(1.5f));
    }
}


// ===========================================================================
// Frames
// ===========================================================================

FrameSpec RenderSession::frame_spec(double t, int W, int H, bool photo) {
    FrameSpec f;
    f.cam = trajectory().at(t);
    f.width = W;
    f.height = H;
    for (int k = 0; k < 3; k++) f.up[k] = _project.up[k];
    for (int k = 0; k < 3; k++) f.background[k] = _project.background[k];
    const bool png = _project.output.format == ImageFormat::Png;
    f.transparent = photo && png && _project.output.transparent;
    for (const Source& s : _project.sources) f.styles.push_back(s.style);

    // Which shot, and how far into its transition.
    std::vector<Shot> shots = _project.shots;
    if (shots.empty()) shots.push_back(Shot{});
    int j = 0;
    for (int i = 0; i < (int)shots.size(); i++)
        if (shots[(size_t)i].start <= t) j = i;
    const Shot& sh = shots[(size_t)j];
    const int B = sh.source < (int)_sources.size() ? sh.source : -1;
    const int A = j > 0 ? std::min(shots[(size_t)j - 1].source, (int)_sources.size() - 1) : -1;
    double p = sh.duration > 1e-6 ? (t - sh.start) / sh.duration : 1.0;
    if (t < sh.start) p = 1.0;
    if (photo && sh.transition != Transition::Cut) p = std::clamp(p, 0.0, 1.0);
    f.b.source = B;
    f.mix = 1.0f;
    if (p < 1.0 && sh.transition != Transition::Cut) {
        const double u = std::clamp(p, 0.0, 1.0);
        switch (sh.transition) {
            case Transition::Crossfade:
                f.a.source = A;
                f.mix = (float)smooth(u);
                break;
            case Transition::DipBlack:
            case Transition::DipWhite: {
                const float c = sh.transition == Transition::DipWhite ? 1.0f : 0.0f;
                f.tint[0][0] = f.tint[0][1] = f.tint[0][2] = c;
                if (u < 0.5) { f.b.source = A; f.tint[0][3] = (float)smooth(u * 2.0); }
                else f.tint[0][3] = (float)(1.0 - smooth(u * 2.0 - 1.0));
                break;
            }
            case Transition::WipeLeft:
            case Transition::WipeRight:
            case Transition::WipeUp:
            case Transition::WipeDown: {
                static const float dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                const int d = (int)sh.transition - (int)Transition::WipeLeft;
                f.a.source = A;
                f.mask = 1;
                f.mix = (float)u;
                f.wipe_dir[0] = -dirs[d][0];
                f.wipe_dir[1] = -dirs[d][1];
                break;
            }
            case Transition::Iris:
                f.a.source = A;
                f.mask = 2;
                f.mix = (float)smooth(u);
                break;
            case Transition::Sweep: {
                // Swept over the model coming in: it is the one the cut
                // should be seen to travel up.
                double r[2], lo = 0.0, hi = 0.0;
                bool have = false;
                if ((B >= 0 && _frames.height_range(B, _project.up, r)) ||
                    (A >= 0 && _frames.height_range(A, _project.up, r))) {
                    lo = r[0];
                    hi = r[1];
                    have = true;
                }
                if (!have) {
                    f.a.source = A;
                    f.mix = (float)smooth(u);
                    break;
                }
                const double span = hi - lo, margin = 0.08 * span;
                const double level = lo - margin + (span + 2.0 * margin) * smooth(u);
                f.mode = 1;
                f.a.source = A;
                f.a.clip = 2;
                f.b.clip = 1;
                f.a.level = f.b.level = level;
                f.a.glow = f.b.glow = (float)(0.04 * span);
                break;
            }
            case Transition::Grow:
                f.a.source = A;
                f.mix = (float)smooth(std::min(1.0, u * 1.5));
                f.b.grow = (float)(0.02 + 0.98 * smooth(u));
                f.b.fade_in = (float)std::min(1.0, u * 2.5);
                break;
            default:
                break;
        }
    }

    // The fades, over everything; a photo has none.
    if (!photo) {
        const double T = _project.duration();
        auto fade = [&](const Fade& fd, double into) {
            if (fd.colour == FadeColour::None || fd.seconds <= 1e-6) return;
            const double amount = 1.0 - smooth(into / fd.seconds);
            if (amount <= 0.0) return;
            const float c = fd.colour == FadeColour::White ? 1.0f : 0.0f;
            if (amount > f.tint[1][3]) {
                f.tint[1][0] = f.tint[1][1] = f.tint[1][2] = c;
                f.tint[1][3] = (float)amount;
            }
        };
        fade(_project.fade_in, t - (_project.keys.empty() ? 0.0 : _project.keys.front().time));
        fade(_project.fade_out, T - t);
    }
    return f;
}

void RenderSession::preview_size(float box_w, float box_h, int& W, int& H) const {
    const double aspect = (double)_project.output.width / std::max(_project.output.height, 1);
    double w = std::max(64.0f, box_w), h = w / aspect;
    if (h > box_h && box_h > 32.0f) { h = box_h; w = h * aspect; }
    // Never more than the output, and never more than the scale asks for.
    const double cap = std::max(0.1, (double)_preview_scale) * _project.output.width;
    if (w > cap) { w = cap; h = w / aspect; }
    W = std::max(32, (int)std::lround(w));
    H = std::max(18, (int)std::lround(h));
}

void RenderSession::request_preview() {
    if (!_have_project || _project.keys.empty() || exporting() || _frames.busy()) return;
    if (_frames.source_count() == 0) return;
    int W = _preview_w, H = _preview_h;
    if (W <= 0 || H <= 0) preview_size(px(420.0f), px(240.0f), W, H);
    char key[96];
    std::snprintf(key, sizeof key, "%llu|%.6f|%dx%d", (unsigned long long)_revision,
                  _time, W, H);
    if (key == _preview_key) return;
    _preview_key = key;
    const bool photo = _project.output.kind == OutputKind::Photo;
    _frames.request(frame_spec(_time, W, H, photo));
}


// ===========================================================================
// Once a frame
// ===========================================================================

void RenderSession::poll() {
    if (!_panel) return;
    if (!_have_project) new_project();
    if (_fly_block_key && !ImGui::IsKeyDown((ImGuiKey)_fly_block_key)) _fly_block_key = 0;
    _sel.resize(_project.keys.size(), 0);

    if (_playing) {
        const double T = _project.duration();
        const double first = _project.keys.empty() ? 0.0 : _project.keys.front().time;
        _time = _play_from + (now_s() - _play_clock);
        if (_time >= T) {
            if (_loop && T > first) {
                _play_from = first;
                _play_clock = now_s();
                _time = first;
            } else {
                _time = T;
                _playing = false;
            }
        }
    }

    if (_pick_waiting) {
        float p[3];
        bool hit = false;
        if (_panel->take_pick(p, hit)) {
            _pick_waiting = false;
            if (hit) {
                double s[3] = {p[0], p[1], p[2]}, w[3];
                _w2s.inverse().apply(s, w);
                bool any = false;
                for (uint8_t v : _sel) any = any || v;
                for (int i = 0; i < (int)_project.keys.size(); i++) {
                    if (any && !selected(i)) continue;
                    Keyframe& k = _project.keys[(size_t)i];
                    k.aim = true;
                    for (int d = 0; d < 3; d++) k.target[d] = w[d];
                    k.roll = 0.0;
                    update_aim(k, _project.up);
                }
                project_changed();
            } else {
                note(msg::pick_missed.get());
            }
        }
    }

    if (_job.state != Job::Idle) poll_export();
    else {
        _frames.poll(0.0);
        request_preview();
    }
    const std::string err = _frames.take_error();
    if (!err.empty()) {
        _status = format(msg::render_error, {err});
        _status_err = true;
    }
    commit_history();
}


// ===========================================================================
// Export
// ===========================================================================

// Asks `spirula encode --probe` which codecs this GPU really encodes -- it
// encodes two frames with each, since a device can list a codec and still
// refuse a session.
void RenderSession::probe_encoder() {
#ifdef SS_TOOL_ENCODE
    _encoder_probe = 1;
    _probe = std::thread([this] {
        std::atomic<bool> cancel{false};
        int codecs = 0;
        const int code = run_process({app::exe_path(), "encode", "--probe"}, "",
                                     [&](const std::string& line) {
                                         if (line == "h264") codecs |= 1;
                                         if (line == "h265") codecs |= 2;
                                     }, cancel);
        _encoder_codecs = codecs;
        _encoder_probe = code == 0 && codecs ? 2 : 3;
    });
#else
    _encoder_probe = 3;
#endif
}

bool RenderSession::builtin_encodes(bool h265) const {
    return _encoder_probe.load() == 2 && (_encoder_codecs.load() & (h265 ? 2 : 1));
}

Encoder RenderSession::pick_encoder() {
    Encoder e;
    if (builtin_encodes(_project.output.codec == Codec::H265)) {
        e.kind = Encoder::BuiltIn;
        e.exe = app::exe_path();
    } else if (command_exists(_ffmpeg)) {
        e.kind = Encoder::Ffmpeg;
        e.exe = _ffmpeg;
    }
    return e;
}

void RenderSession::start_export() {
    if (_job.state != Job::Idle || _project.keys.empty()) return;
    Output& o = _project.output;
    const bool photo = o.kind == OutputKind::Photo;
    if (o.path.empty()) {
        if (_pick) _pick(Pick::Output, default_project_dir(_sources.empty() ? "" : _sources[0].path), "");
        return;
    }
    const double T = _project.duration();
    if (!photo && T <= 0.0) {
        note(msg::need_two_keys.get());
        _status_err = true;
        return;
    }
    _playing = false;
    if (_job.finisher.joinable()) _job.finisher.join();
    _job.reset();
    int W = std::max(16, o.width), H = std::max(16, o.height);
    if (o.kind == OutputKind::Video) { W += W & 1; H += H & 1; }
    _job.photo = photo;
    _job.frames = photo ? 1 : std::max(1, (int)std::lround(T * o.fps) + 1);
    _job.path = o.path;
    if (o.kind == OutputKind::Video) {
        std::error_code ec;
        const fs::path parent = fs::u8path(o.path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        const Encoder e = pick_encoder();
        if (e.kind == Encoder::None) {
            _status = msg::no_encoder.get();
            _status_err = true;
            return;
        }
        const bool sphere = !_project.keys.empty() &&
                            _project.lens_at(0).projection == Projection::Equirect;
        _job.sink = open_pipe_sink(encoder_argv(e, W, H, o.fps, o.codec == Codec::H265,
                                                o.quality, sphere, o.path), W, H);
    } else {
        const bool jpeg = o.format == ImageFormat::Jpeg;
        std::string path = o.path;
        if (o.kind == OutputKind::Frames) {
            // A folder: the frames are numbered inside it.
            path = (fs::u8path(o.path) / (jpeg ? "frame_%05d.jpg" : "frame_%05d.png")).string();
        }
        _job.sink = open_image_sink(path, W, H, jpeg, o.jpeg_quality,
                                    !jpeg && o.transparent);
    }
    _job.state = Job::Preparing;
    _job.started = now_s();
    _status.clear();
    _status_err = false;
}

void RenderSession::cancel_export() {
    if (_job.state == Job::Idle) return;
    if (_job.finisher.joinable()) _job.finisher.join();
    if (_job.sink) _job.sink->cancel();
    if (!_job.photo && _project.output.kind == OutputKind::Video) {
        std::error_code ec;
        fs::remove(fs::u8path(_job.path), ec);
    }
    _job.sink.reset();
    _job.state = Job::Idle;
    _preview_key.clear();
    note(msg::render_cancelled.get());
}

void RenderSession::poll_export() {
    Job& j = _job;
    const Output& o = _project.output;
    int W = std::max(16, o.width), H = std::max(16, o.height);
    if (o.kind == OutputKind::Video) { W += W & 1; H += H & 1; }
    if (j.state == Job::Preparing) {
        // Effects that rewrite splats need them read first.
        bool ready = true;
        for (int i = 0; i < _frames.source_count(); i++) ready = _frames.effects_ready(i) && ready;
        if (_frames.busy()) {
            _frames.poll(0.0);
            ready = false;
        }
        if (!ready) return;
        j.state = Job::Rendering;
        j.frame = 0;
    }
    if (j.state == Job::Rendering) {
        const double budget = now_s() + 0.035;
        const double first = _project.keys.front().time;
        do {
            if (!_frames.busy()) {
                const double t = j.photo ? _time
                                         : std::min(first + j.frame / o.fps, _project.duration());
                _frames.request(frame_spec(t, W, H, j.photo));
            }
            if (!_frames.poll(0.02)) continue;
            std::vector<uint8_t> px;
            _frames.read(px, j.sink->channels() == 4);
            if (!j.sink->push(std::move(px))) {
                j.ok = false;
                j.error = j.sink->error();
                j.frame = j.frames;
            } else {
                j.frame++;
            }
            if (j.frame >= j.frames) {
                j.state = Job::Finishing;
                j.finished = false;
                FrameSink* sink = j.sink.get();
                Job* jp = &j;
                j.finisher = std::thread([sink, jp] {
                    const bool ok = sink->finish();
                    if (!ok) {
                        jp->ok = false;
                        if (jp->error.empty()) jp->error = sink->error();
                    }
                    jp->finished = true;
                });
                break;
            }
        } while (now_s() < budget);
    }
    if (j.state == Job::Finishing && j.finished.load()) {
        j.finisher.join();
        j.sink.reset();
        j.state = Job::Idle;
        _preview_key.clear();
        if (j.ok) {
            note(format(msg::render_saved, {j.path}));
        } else {
            _status = format(msg::render_failed, {j.error});
            _status_err = true;
            _log.push_back(_status);
        }
    }
}

}  // namespace gui::render
