// RenderTimeline.cpp -- the strip under the panes (transport, ruler, keys,
// shots) and the camera's own picture, for the session in RenderSession.h.

#include "app/gui/render/RenderSession.h"

#include "app/gui/Ui.h"
#include "i18n/catalog/Render.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace msg = spirula::i18n::msg::render;
using spirula::i18n::Msg;
using spirula::i18n::format;

namespace gui::render {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr ImU32 kShotCols[4] = {IM_COL32(70, 110, 160, 255), IM_COL32(120, 90, 160, 255),
                                IM_COL32(80, 140, 100, 255), IM_COL32(160, 110, 70, 255)};

// A step on the ruler that leaves about `px_per` pixels between labels.
double nice_step(double seconds_per_px, double px_per) {
    const double raw = seconds_per_px * px_per;
    const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    for (double s : steps)
        if (s >= raw) return s;
    return 600.0;
}

void axis_rotate(const double axis[3], double angle, const double v[3], double out[3]) {
    const double origin[3] = {0, 0, 0};
    spirula::Sim3::rotation_about(axis, angle, origin).rotate(v, out);
}

}  // namespace

float RenderSession::timeline_height() const {
    return ImGui::GetFrameHeightWithSpacing() + px(58.0f) + ImGui::GetStyle().ItemSpacing.y;
}

void RenderSession::draw_timeline() {
    if (!_panel || !_have_project) return;
    const ImGuiStyle& st = ImGui::GetStyle();
    const double T = std::max(_project.duration(), 1e-3);
    const double first = _project.keys.empty() ? 0.0 : _project.keys.front().time;

    // ---- transport ----
    auto jump = [&](double t) { _time = std::clamp(t, 0.0, T); _playing = false; };
    if (ui::ButtonRaw("|<##start")) jump(first);
    ui::help_on_hover(msg::tl_start);
    ImGui::SameLine();
    const std::vector<double> visits = trajectory().key_times();
    if (ui::ButtonRaw("<##prevkey")) {
        double best = first;
        for (double t : visits) if (t < _time - 1e-6) best = t;
        jump(best);
    }
    ui::help_on_hover(msg::tl_prev_key);
    ImGui::SameLine();
    if (ui::KeyButton(_playing ? msg::tl_pause : msg::tl_play, px(80.0f), nullptr, _playing))
        set_playing(!_playing);
    ui::help_on_hover(msg::tl_play_help);
    ImGui::SameLine();
    if (ui::ButtonRaw(">##nextkey")) {
        double best = T;
        for (double t : visits)
            if (t > _time + 1e-6) { best = t; break; }
        jump(best);
    }
    ui::help_on_hover(msg::tl_next_key);
    ImGui::SameLine();
    if (ui::ButtonRaw(">|##end")) jump(T);
    ui::help_on_hover(msg::tl_end);
    ImGui::SameLine();
    char now[64];
    const double fps = std::max(_project.output.fps, 1.0);
    std::snprintf(now, sizeof now, "%6.2f / %.2f s   #%d", _time, _project.duration(),
                  std::max(1, (int)std::lround((_time - first) * fps) + 1));
    ui::TextRaw(now);
    ImGui::SameLine();
    ui::Checkbox(msg::tl_loop, &_loop);

    // Preview: where the camera's picture goes, and how sharp it is drawn.
    const Msg* modes[3] = {&msg::pv_corner, &msg::pv_beside, &msg::pv_through};
    float modes_w = 0.0f;
    for (const Msg* m : modes)
        modes_w += ImGui::CalcTextSize(m->get()).x + 2.0f * st.FramePadding.x + st.ItemSpacing.x;
    const float scale_w = px(90.0f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                             ImGui::GetContentRegionMax().x - modes_w - scale_w));
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        const bool on = (int)_preview_mode == i;
        if (ui::KeyButton(*modes[i], 0.0f, nullptr, on) && !on) {
            _preview_mode = (PreviewMode)i;
            _preview_key.clear();
        }
        ui::help_on_hover(msg::pv_help);
    }
    ImGui::SameLine();
    int q = _preview_scale <= 0.26f ? 0 : _preview_scale <= 0.51f ? 1 : 2;
    const char* qs[] = {"25%", "50%", "100%"};
    ImGui::SetNextItemWidth(scale_w);
    if (ui::ComboRaw("##pvscale", &q, qs, 3)) {
        _preview_scale = q == 0 ? 0.25f : q == 1 ? 0.5f : 1.0f;
        _preview_key.clear();
    }
    ui::help_on_hover(msg::pv_scale_help);

    // ---- the track ----
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = px(58.0f);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ui::InvisibleButtonRaw("##track", ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    _timeline_hovered = hovered;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = px(10.0f);
    const double span = T * 1.04 + 1e-6;
    auto x_of = [&](double t) { return a.x + pad + (float)(t / span) * (w - 2.0f * pad); };
    auto t_of = [&](float x) {
        return std::clamp((double)(x - a.x - pad) / std::max(w - 2.0f * pad, 1.0f) * span, 0.0, T);
    };
    dl->AddRectFilled(a, b, IM_COL32(22, 24, 28, 255), px(3.0f));

    // Ruler.
    const float ruler_y = a.y + px(14.0f);
    const double step = nice_step(span / std::max(w, 1.0f), 70.0);
    for (double t = 0.0; t <= span; t += step) {
        const float x = x_of(t);
        dl->AddLine(ImVec2(x, a.y + px(2.0f)), ImVec2(x, ruler_y), IM_COL32(120, 120, 130, 255));
        char b2[16];
        std::snprintf(b2, sizeof b2, step < 1.0 ? "%.1f" : "%.0f", t);
        dl->AddText(ImVec2(x + px(3.0f), a.y), IM_COL32(150, 150, 160, 255), b2);
    }

    // Shots: what is on screen when, and where each one arrives.
    const float lane0 = b.y - px(16.0f), lane1 = b.y - px(4.0f);
    const std::vector<Shot>& shots = _project.shots;
    for (size_t i = 0; i < shots.size(); i++) {
        const double s0 = shots[i].start;
        const double s1 = i + 1 < shots.size() ? shots[i + 1].start : T;
        const float x0 = x_of(std::min(s0, T)), x1 = x_of(std::min(s1, T));
        if (x1 <= x0) continue;
        const ImU32 col = shots[i].source < 0 ? IM_COL32(50, 50, 55, 255)
                                              : kShotCols[(size_t)std::max(0, shots[i].source) % 4];
        dl->AddRectFilled(ImVec2(x0, lane0), ImVec2(x1, lane1), col, px(2.0f));
        if (shots[i].transition != Transition::Cut) {
            const float xt = x_of(std::min(s0 + shots[i].duration, T));
            dl->AddRectFilledMultiColor(ImVec2(x0, lane0), ImVec2(xt, lane1),
                                        IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0),
                                        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90));
        }
        const int src = shots[i].source;
        if (src >= 0 && src < (int)_sources.size())
            dl->AddText(ImVec2(x0 + px(4.0f), lane0 - px(1.0f)), IM_COL32(230, 230, 230, 220),
                        _sources[(size_t)src].name.c_str());
    }
    // Fades, as the ramps an editor draws them as.
    if (_project.fade_in.colour != FadeColour::None) {
        const float x1 = x_of(first + _project.fade_in.seconds);
        dl->AddTriangleFilled(ImVec2(x_of(first), ruler_y + px(2.0f)), ImVec2(x_of(first), lane0),
                              ImVec2(x1, lane0), IM_COL32(255, 255, 255, 40));
    }
    if (_project.fade_out.colour != FadeColour::None) {
        const float x0 = x_of(T - _project.fade_out.seconds);
        dl->AddTriangleFilled(ImVec2(x_of(T), ruler_y + px(2.0f)), ImVec2(x_of(T), lane0),
                              ImVec2(x0, lane0), IM_COL32(255, 255, 255, 40));
    }

    // Keys.
    const float key_y = (ruler_y + lane0) * 0.5f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    for (int i = 0; i < (int)_project.keys.size(); i++) {
        const float x = x_of(visits[(size_t)i]);
        if (hovered && std::fabs(mouse.x - x) < px(7.0f) && std::fabs(mouse.y - key_y) < px(9.0f))
            hot = i;
    }
    for (int i = 0; i < (int)_project.keys.size(); i++) {
        const Keyframe& k = _project.keys[(size_t)i];
        const float x = x_of(visits[(size_t)i]), r = px(6.0f);
        const ImU32 col = selected(i) ? IM_COL32(255, 150, 40, 255)
                          : i == hot ? IM_COL32(255, 235, 90, 255) : IM_COL32(220, 220, 220, 255);
        const ImVec2 pts[4] = {ImVec2(x, key_y - r), ImVec2(x + r, key_y),
                               ImVec2(x, key_y + r), ImVec2(x - r, key_y)};
        dl->AddConvexPolyFilled(pts, 4, col);
        if (k.hold) dl->AddRect(ImVec2(x - r, key_y - r), ImVec2(x + r, key_y + r), col, 0, 0, px(1.0f));
        if (k.own_lens && i > 0) dl->AddCircle(ImVec2(x, key_y), r + px(3.0f), col, 12, px(1.0f));
    }
    // A loop comes back to its first key: drawn there, hollow.
    if (_project.looped()) {
        const float x = x_of(T), r = px(6.0f);
        const ImVec2 pts[4] = {ImVec2(x, key_y - r), ImVec2(x + r, key_y),
                               ImVec2(x, key_y + r), ImVec2(x - r, key_y)};
        dl->AddPolyline(pts, 4, IM_COL32(220, 220, 220, 200), ImDrawFlags_Closed, px(1.5f));
    }

    // The playhead.
    const float xh = x_of(std::min(_time, T));
    dl->AddLine(ImVec2(xh, a.y), ImVec2(xh, b.y), IM_COL32(90, 220, 255, 255), px(2.0f));
    dl->AddTriangleFilled(ImVec2(xh - px(5.0f), a.y), ImVec2(xh + px(5.0f), a.y),
                          ImVec2(xh, a.y + px(7.0f)), IM_COL32(90, 220, 255, 255));

    // Input: a key is picked and dragged in time; anywhere else scrubs.
    const double frame = 1.0 / fps;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hot >= 0) {
            const ImGuiIO& io = ImGui::GetIO();
            // A plain click on a selected key keeps the group, to drag it.
            if (io.KeyShift || io.KeyCtrl || !selected(hot)) click_select(hot, io.KeyCtrl, io.KeyShift);
            // At constant speed only the ends have a time of their own.
            const bool fixed = _project.motion.constant_speed && hot > 0 &&
                               hot + 1 < (int)_project.keys.size();
            _drag_key = fixed ? -1 : hot;
            _drag_key_from = _project.keys[(size_t)hot].time;
            _time = visits[(size_t)hot];
        } else {
            _scrubbing = true;
            _playing = false;
        }
    }
    if (active && _drag_key >= 0 && _drag_key < (int)_project.keys.size() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const double dt = t_of(mouse.x) - t_of(ImGui::GetIO().MouseClickedPos[0].x);
        const double moved = std::round((_drag_key_from + dt) / frame) * frame;
        // Every selected key shifts together, so a group keeps its spacing.
        const double shift = std::max(0.0, moved) - _project.keys[(size_t)_drag_key].time;
        if (std::fabs(shift) > 1e-9) {
            for (int i = 0; i < (int)_project.keys.size(); i++)
                if (selected(i) || i == _drag_key)
                    _project.keys[(size_t)i].time = std::max(0.0, _project.keys[(size_t)i].time + shift);
            const double keep = _project.keys[(size_t)_drag_key].time;
            std::vector<uint8_t> sel = _sel;
            std::vector<int> order;
            for (int i = 0; i < (int)_project.keys.size(); i++) order.push_back(i);
            const std::vector<Keyframe> was = _project.keys;
            std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
                return was[(size_t)x].time < was[(size_t)y].time;
            });
            _sel.resize(order.size(), 0);
            int dragged = _drag_key;
            for (size_t i = 0; i < order.size(); i++) {
                _project.keys[i] = was[(size_t)order[i]];
                _sel[i] = (size_t)order[i] < sel.size() ? sel[(size_t)order[i]] : 0;
                if (order[i] == _drag_key) dragged = (int)i;
            }
            _drag_key = dragged;
            // The first key always has a lens of its own: the one it showed.
            if (!_project.keys[0].own_lens) {
                RenderProject p = _project;
                p.keys = was;
                _project.keys[0].lens = p.lens_at(order[0]);
                _project.keys[0].own_lens = true;
            }
            _time = keep;
            project_changed();
        }
    }
    if (_scrubbing && active) _time = std::round(t_of(mouse.x) / frame) * frame;
    if (!active) {
        _drag_key = -1;
        _scrubbing = false;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0)
        add_key(std::round(t_of(mouse.x) / frame) * frame, true);
    if (hovered && hot >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!selected(hot)) select_only(hot);
        ImGui::OpenPopup("##keymenu");
    }
    if (ImGui::BeginPopup("##keymenu")) {
        const int one = single_selected();
        if (ui::MenuItem(msg::key_look_through, "0")) look_through(_time);
        if (one >= 0) {
            bool hold = _project.keys[(size_t)one].hold;
            if (ui::MenuItem(msg::key_hold, nullptr, &hold)) {
                _project.keys[(size_t)one].hold = hold;
                project_changed();
            }
            if (ui::MenuItem(msg::key_update)) update_key_from_view(one);
        }
        if (ui::MenuItem(msg::key_delete, "X")) delete_selected();
        ImGui::EndPopup();
    }
    ui::help_on_hover(msg::tl_track_help);
}

// The camera's own view, and in Through mode where it is flown from. What
// moves is the key at the playhead, made there if there is none.
void RenderSession::draw_preview_pane() {
    if (!_panel || !_have_project) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    int W, H;
    preview_size(avail.x, avail.y - ImGui::GetTextLineHeightWithSpacing(), W, H);
    _preview_w = W;
    _preview_h = H;
    const double aspect = (double)_project.output.width / std::max(_project.output.height, 1);
    float dw = avail.x, dh = (float)(avail.x / aspect);
    const float room = avail.y - ImGui::GetTextLineHeightWithSpacing();
    if (dh > room) { dh = room; dw = (float)(dh * aspect); }
    // In the middle of what there is, the caption under it.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(at.x + (avail.x - dw) * 0.5f,
                                     at.y + std::max(0.0f, (room - dh) * 0.5f)));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ui::InvisibleButtonRaw("##camview", ImVec2(std::max(dw, 1.0f), std::max(dh, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + dw, a.y + dh);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 255));
    if (_frames.texture())
        dl->AddImage((ImTextureID)(intptr_t)_frames.texture(), a, b, ImVec2(0, 1), ImVec2(1, 0));
    dl->AddRect(a, b, IM_COL32(90, 220, 255, 160));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + ImGui::GetStyle().ItemSpacing.y));
    ui::TextDisabled(_preview_mode == PreviewMode::Through ? msg::pv_through_hint : msg::pv_caption,
                     {(long long)_project.output.width, (long long)_project.output.height});
    if (_preview_mode != PreviewMode::Through || _project.keys.empty()) return;

    const ImGuiIO& io = ImGui::GetIO();
    const bool turn = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && !io.KeyShift;
    const bool slide = active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                                  (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)));
    const float wheel = hovered ? io.MouseWheel : 0.0f;
    if (!turn && !slide && wheel == 0.0f) return;
    if (io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f && wheel == 0.0f) return;

    // The key at the playhead, or a new one there holding what is seen now.
    const double frame = 1.0 / std::max(_project.output.fps, 1.0);
    int k = -1;
    const std::vector<double> visits = trajectory().key_times();
    for (int i = 0; i < (int)visits.size(); i++)
        if (std::fabs(visits[(size_t)i] - _time) < 0.5 * frame) k = i;
    if (k < 0) {
        const CameraState c = trajectory().at(_time);
        Keyframe nk;
        nk.time = _time;
        for (int d = 0; d < 3; d++) nk.pos[d] = c.pos[d];
        for (int d = 0; d < 4; d++) nk.rot[d] = c.rot[d];
        _project.keys.push_back(nk);
        _project.sort_keys();
        for (int i = 0; i < (int)_project.keys.size(); i++)
            if (_project.keys[(size_t)i].time == _time) k = i;
        _sel.insert(_sel.begin() + k, 0);
    }
    select_only(k);
    Keyframe& key = _project.keys[(size_t)k];
    double R[9];
    quat_to_matrix3(key.rot, R);
    const double right[3] = {R[0], R[3], R[6]}, up[3] = {R[1], R[4], R[7]};
    const double back[3] = {R[2], R[5], R[8]};
    const double reach = key.aim ? std::sqrt((key.pos[0]-key.target[0])*(key.pos[0]-key.target[0]) +
                                             (key.pos[1]-key.target[1])*(key.pos[1]-key.target[1]) +
                                             (key.pos[2]-key.target[2])*(key.pos[2]-key.target[2]))
                                 : 1.0 / std::max(_w2s.s, 1e-12);
    const float dx = io.MouseDelta.x, dy = io.MouseDelta.y;
    if (turn) {
        const double yaw = -dx * 0.004, pitch = -dy * 0.004;
        if (key.aim) {
            // An aimed camera goes round what it looks at.
            double rel[3] = {key.pos[0]-key.target[0], key.pos[1]-key.target[1], key.pos[2]-key.target[2]};
            double t1[3], t2[3];
            axis_rotate(_project.up, yaw, rel, t1);
            axis_rotate(right, pitch, t1, t2);
            for (int d = 0; d < 3; d++) key.pos[d] = key.target[d] + t2[d];
        } else {
            spirula::Sim3 rot = spirula::Sim3::rotation_about(_project.up, yaw, key.pos);
            spirula::Sim3 tilt = spirula::Sim3::rotation_about(right, pitch, key.pos);
            spirula::Sim3 both = rot * tilt;
            double M[9];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) {
                    double v = 0.0;
                    for (int m = 0; m < 3; m++) v += both.R[r*3+m] * R[m*3+c];
                    M[r*3+c] = v;
                }
            quat_from_matrix3(M, key.rot);
        }
    }
    if (slide) {
        const double s = reach * 0.0015;
        for (int d = 0; d < 3; d++) {
            const double m = (-dx * right[d] + dy * up[d]) * s;
            key.pos[d] += m;
            if (key.aim) key.target[d] += m;
        }
    }
    if (wheel != 0.0f) {
        if (io.KeyCtrl) {
            if (!key.own_lens) { key.lens = _project.lens_at(k); key.own_lens = true; }
            if (key.lens.projection != Projection::Equirect)
                lens_set_fov(key.lens, std::clamp(lens_fov(key.lens) * std::exp(-wheel * 0.06), 5.0,
                                                  key.lens.projection == Projection::Perspective ? 150.0 : 360.0));
        } else {
            const double s = reach * 0.08 * wheel;
            for (int d = 0; d < 3; d++) key.pos[d] -= back[d] * s;
        }
    }
    update_aim(key, _project.up);
    project_changed();
    (void)kPi;
}

}  // namespace gui::render
