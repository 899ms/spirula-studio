// RenderPanel.cpp -- the render mode's panel, and the keys that drive it, for
// the session in RenderSession.h.

#include "app/gui/render/RenderSession.h"

#include "app/gui/Subprocess.h"
#include "app/gui/Ui.h"
#include "app/gui/ViewportPanel.h"
#include "data/DatasetParser.h"
#include "i18n/catalog/Render.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
namespace msg = spirula::i18n::msg::render;
using spirula::i18n::Msg;
using spirula::i18n::format;

namespace gui::render {

namespace {

constexpr double kPi = 3.14159265358979323846;
const ImVec4 kErr(1.0f, 0.45f, 0.45f, 1.0f);
const ImVec4 kDim(0.62f, 0.64f, 0.68f, 1.0f);

struct Resolution { int w, h; const Msg* name; };
const Resolution kResolutions[] = {
    {1280, 720, &msg::res_hd}, {1920, 1080, &msg::res_full_hd},
    {2560, 1440, &msg::res_qhd}, {3840, 2160, &msg::res_4k},
    {1080, 1920, &msg::res_vertical}, {1080, 1080, &msg::res_square},
    {3840, 1920, &msg::res_360_4k}, {5760, 2880, &msg::res_360_6k},
    {7680, 3840, &msg::res_360_8k},
};
constexpr int kNumResolutions = (int)(sizeof kResolutions / sizeof kResolutions[0]);

const Msg* const kProjections[kNumProjections] = {
    &msg::proj_perspective, &msg::proj_fisheye, &msg::proj_equisolid,
    &msg::proj_equirect};
const Msg* const kTransitions[kNumTransitions] = {
    &msg::tr_cut, &msg::tr_crossfade, &msg::tr_dip_black, &msg::tr_dip_white,
    &msg::tr_wipe_left, &msg::tr_wipe_right, &msg::tr_wipe_up, &msg::tr_wipe_down,
    &msg::tr_iris, &msg::tr_sweep, &msg::tr_grow};
const Msg* const kPointStyles[kNumPointStyles] = {
    &msg::pt_square, &msg::pt_circle, &msg::pt_gaussian, &msg::pt_sphere};

// Keys the whole panel answers to, beside the viewport's own. A key is an
// identifier, so it never gets translated.
struct KeyRow { const char* label; ImGuiKey key; bool ctrl, shift, alt; };

std::string preset_label(const LensPreset& p) {
    return p.arg.empty() ? std::string(p.name->get()) : format(*p.name, {p.arg});
}

std::string seconds(double t) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f s", t);
    return b;
}

// One path serves all three outputs; it follows the kind so a photo is never
// written under a video's name.
void fit_output_path(Output& o) {
    if (o.path.empty()) return;
    fs::path p = fs::u8path(o.path);
    const std::string ext = p.extension().string();
    if (o.kind == OutputKind::Frames) {
        if (!ext.empty()) p.replace_extension();
    } else if (o.kind == OutputKind::Video) {
        if (ext != ".mp4" && ext != ".h264" && ext != ".h265") p.replace_extension(".mp4");
    } else {
        const char* want = o.format == ImageFormat::Jpeg ? ".jpg" : ".png";
        if (ext != want && !(o.format == ImageFormat::Jpeg && ext == ".jpeg"))
            p.replace_extension(want);
    }
    o.path = p.string();
}

bool combo_msgs(const char* id, int* cur, const Msg* const* items, int n) {
    std::vector<const Msg*> v(items, items + n);
    return ui::ComboRaw(id, cur, v);
}

}  // namespace


// ===========================================================================
// Keys
// ===========================================================================

void RenderSession::handle_keys(bool over_view) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || _xform.active()) return;
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeyShift;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };

    if (io.KeyCtrl && !io.KeyShift && pressed(ImGuiKey_Z)) { undo(); return; }
    if (io.KeyCtrl && (pressed(ImGuiKey_Y) || (io.KeyShift && pressed(ImGuiKey_Z)))) {
        redo();
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_S)) {
        if (_project_path.empty()) {
            if (_pick) _pick(Pick::SaveProject, default_project_dir(_sources.empty() ? "" : _sources[0].path), "camera.json");
        } else {
            save_to(_project_path);
        }
        return;
    }
    if (io.KeyCtrl && pressed(ImGuiKey_A)) {
        _sel.assign(_project.keys.size(), 1);
        return;
    }
    if (plain && pressed(ImGuiKey_Space)) {
        _playing = !_playing;
        if (_playing) {
            if (_time >= _project.duration() - 1e-6 && !_project.keys.empty())
                _time = _project.keys.front().time;
            _play_from = _time;
            _play_clock = ImGui::GetTime();
        }
        return;
    }
    const double frame = 1.0 / std::max(_project.output.fps, 1.0);
    if (plain && pressed(ImGuiKey_Home) && !_project.keys.empty()) _time = _project.keys.front().time;
    if (plain && pressed(ImGuiKey_End)) _time = _project.duration();
    if (plain && ImGui::IsKeyPressed(ImGuiKey_Period, true)) _time = std::min(_time + frame, _project.duration());
    if (plain && ImGui::IsKeyPressed(ImGuiKey_Comma, true)) _time = std::max(_time - frame, 0.0);
    if (plain && (pressed(ImGuiKey_PageDown) || pressed(ImGuiKey_PageUp))) {
        const bool next = pressed(ImGuiKey_PageDown);
        double best = _time;
        for (const Keyframe& k : _project.keys) {
            if (next && k.time > _time + 1e-6 && (best == _time || k.time < best)) best = k.time;
            if (!next && k.time < _time - 1e-6 && (best == _time || k.time > best)) best = k.time;
        }
        _time = best;
    }
    if (!over_view) return;

    // Letters the camera also flies with are the camera's until a key is
    // selected, which is what G / R / S then act on.
    const bool letters = blocks_fly_keys();
    if (plain && (pressed(ImGuiKey_K) || pressed(ImGuiKey_I))) {
        const int at = add_key_from_view(_time, true);
        (void)at;
        _fly_block_key = pressed(ImGuiKey_K) ? ImGuiKey_K : ImGuiKey_I;
        return;
    }
    if (plain && (pressed(ImGuiKey_Keypad0) || pressed(ImGuiKey_0))) {
        look_through(_time);
        return;
    }
    if (plain && pressed(ImGuiKey_T)) {
        _pick_target = true;
        return;
    }
    if (!letters) return;
    if (plain && pressed(ImGuiKey_A)) {
        bool all = true;
        for (uint8_t s : _sel) all = all && s;
        _sel.assign(_project.keys.size(), all ? 0 : 1);
        return;
    }
    if (!io.KeyCtrl && !io.KeyShift && io.KeyAlt && pressed(ImGuiKey_A)) {
        _sel.assign(_project.keys.size(), 0);
        return;
    }
    if (plain && (pressed(ImGuiKey_X) || pressed(ImGuiKey_Delete))) {
        delete_selected();
        return;
    }
    const struct { ImGuiKey key; XformKind kind; } ops[] = {
        {ImGuiKey_G, XformKind::Move}, {ImGuiKey_R, XformKind::Rotate},
        {ImGuiKey_S, XformKind::Scale}};
    for (const auto& op : ops)
        if (plain && pressed(op.key)) {
            begin_xform(op.kind, _mouse[0], _mouse[1], false);
            return;
        }
}


// ===========================================================================
// The panel
// ===========================================================================

void RenderSession::draw_status() {
    if (!_have_project) return;
    if (_xform.active()) {
        ui::TextDisabled(_xform.kind() == XformKind::Scale ? msg::hint_op_fov : msg::hint_op);
        return;
    }
    if (_pick_target || _pick_waiting) {
        ui::TextDisabled(msg::hint_pick);
        return;
    }
    bool any = false;
    for (uint8_t s : _sel) any = any || s;
    ui::TextDisabled(any ? msg::hint_selected : msg::hint_idle);
}

void RenderSession::draw_panel() {
    if (!_panel) return;
    handle_keys(_mouse_in);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float strip = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("##renderbody", ImVec2(0, -strip));
    const float full = ImGui::GetContentRegionAvail().x;

    // What comes out: the first choice, since it decides what the rest mean.
    {
        const float third = (full - 2.0f * st.ItemSpacing.x) / 3.0f;
        const Msg* kinds[3] = {&msg::kind_photo, &msg::kind_video, &msg::kind_frames};
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            if (ui::KeyButton(*kinds[i], third, nullptr, (int)_project.output.kind == i) &&
                (int)_project.output.kind != i) {
                _project.output.kind = (OutputKind)i;
                fit_output_path(_project.output);
                project_changed();
            }
            ui::help_on_hover(i == 0 ? msg::kind_photo_help
                              : i == 1 ? msg::kind_video_help : msg::kind_frames_help);
        }
    }
    ImGui::BeginDisabled(exporting());
    if (_project.keys.size() <= 1) {
        ImGui::Spacing();
        if (!_up_known) {
            ui::TextDisabledWrapped(msg::up_unknown);
            if (ui::Button(msg::up_from_view, ImVec2(full, 0))) take_up_from_view();
            ui::help_on_hover(msg::up_from_view_help);
        }
        ui::TextDisabledWrapped(_project.output.kind == OutputKind::Photo
                                    ? msg::start_hint_photo : msg::start_hint_video);
        if (_project.output.kind != OutputKind::Photo) {
            const float half = (full - st.ItemSpacing.x) * 0.5f;
            if (ui::Button(msg::quick_orbit, ImVec2(half, 0))) make_orbit();
            ui::help_on_hover(msg::quick_orbit_help);
            ImGui::SameLine();
            bool have_ds = false;
            for (const SourceInfo& s : _sources)
                have_ds = have_ds || (s.view.kind == SourceView::Points && s.view.ds &&
                                      s.view.ds->num_cameras > 1) ||
                          (s.dataset && s.dataset->num_cameras > 1);
            ImGui::BeginDisabled(!have_ds);
            if (ui::Button(msg::quick_capture, ImVec2(half, 0))) follow_capture();
            ImGui::EndDisabled();
            ui::help_on_hover_disabled(have_ds ? msg::quick_capture_help
                                               : msg::quick_capture_none);
        }
    }

    if (ui::CollapsingHeader(msg::sec_keys, ImGuiTreeNodeFlags_DefaultOpen))
        draw_keys_section(full);
    if (ui::CollapsingHeader(msg::sec_lens, ImGuiTreeNodeFlags_DefaultOpen))
        draw_lens_section(full);
    if (_project.output.kind != OutputKind::Photo && ui::CollapsingHeader(msg::sec_motion))
        draw_motion_section(full);
    if (ui::CollapsingHeader(msg::sec_effects)) draw_effects_section(full);
    if (ui::CollapsingHeader(msg::sec_project)) draw_project_section(full);
    ImGui::EndDisabled();
    if (ui::CollapsingHeader(msg::sec_output, ImGuiTreeNodeFlags_DefaultOpen))
        draw_output_section(full);
    ImGui::EndChild();

    // The way out, and the way to the editor, never scroll away.
    const float half = (full - st.ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(exporting());
    if (ui::Button(msg::to_edit, ImVec2(half, 0)) && _to_edit) _to_edit();
    ui::help_on_hover(msg::to_edit_help);
    ImGui::SameLine();
    if (ui::Button(msg::leave, ImVec2(half, 0)) && _leave) _leave();
    ImGui::EndDisabled();
    if (!_status.empty()) {
        // One line; the whole of it is in the log and on hover.
        const std::string line = elide_middle(_status.substr(0, _status.find('\n')), full);
        if (_status_err) ui::TextColoredRaw(kErr, line);
        else ui::TextDisabledRaw(line);
        if (ImGui::IsItemHovered()) ui::SetTooltipRaw(_status);
    }
}

void RenderSession::draw_keys_section(float full) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float half = (full - st.ItemSpacing.x) * 0.5f;
    if (ui::KeyButton(msg::key_add, half, "K")) add_key_from_view(_time, true);
    ui::help_on_hover(msg::key_add_help);
    ImGui::SameLine();
    const int one = single_selected();
    ImGui::BeginDisabled(one < 0);
    if (ui::Button(msg::key_update, ImVec2(half, 0))) update_key_from_view(one);
    ImGui::EndDisabled();
    ui::help_on_hover(msg::key_update_help);
    if (ui::KeyButton(msg::key_look_through, half, "0")) look_through(_time);
    ui::help_on_hover(msg::key_look_through_help);
    ImGui::SameLine();
    bool any = false;
    for (uint8_t s : _sel) any = any || s;
    ImGui::BeginDisabled(!any);
    if (ui::KeyButton(msg::key_delete, half, "X")) delete_selected();
    ImGui::EndDisabled();

    // The keys as a list: the viewport's cameras, in time order.
    const int n = (int)_project.keys.size();
    if (n > 0 && ImGui::BeginTable("##keys", 4,
                                   ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY,
                                   ImVec2(0, std::min(n, 6) * ImGui::GetFrameHeight() +
                                                 ImGui::GetFrameHeightWithSpacing()))) {
        ui::TableSetupColumnRaw("#", ImGuiTableColumnFlags_WidthFixed, px(26.0f));
        ui::TableSetupColumn(msg::col_time, ImGuiTableColumnFlags_WidthStretch);
        ui::TableSetupColumn(msg::col_lens, ImGuiTableColumnFlags_WidthStretch);
        ui::TableSetupColumn(msg::col_aim, ImGuiTableColumnFlags_WidthFixed, px(48.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++) {
            const Keyframe& k = _project.keys[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            char label[16];
            std::snprintf(label, sizeof label, "%d", i + 1);
            if (ui::SelectableRaw(label, selected(i), ImGuiSelectableFlags_SpanAllColumns)) {
                if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
                    if (i < (int)_sel.size()) _sel[(size_t)i] ^= 1;
                } else {
                    select_only(i);
                }
                _time = k.time;
            }
            ImGui::TableNextColumn();
            ui::TextRaw(seconds(k.time));
            ImGui::TableNextColumn();
            if (k.own_lens || i == 0) {
                char b[48];
                std::snprintf(b, sizeof b, "%.0f\xc2\xb0", lens_fov(k.lens));
                ui::TextRaw(b);
            } else {
                ui::TextDisabled(msg::lens_same);
            }
            ImGui::TableNextColumn();
            if (k.aim) ui::TextRaw("\xe2\x97\x8e");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (one >= 0) {
        Keyframe& k = _project.keys[(size_t)one];
        const float w = full * 0.55f;
        ImGui::SetNextItemWidth(w);
        double t = k.time;
        if (ui::InputDoubleRaw("##ktime", &t, 0.1, 1.0, "%.3f s")) {
            k.time = std::max(0.0, t);
            const double keep = k.time;
            _project.sort_keys();
            for (int i = 0; i < (int)_project.keys.size(); i++)
                if (_project.keys[(size_t)i].time == keep) { select_only(i); break; }
            _time = keep;
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::key_time);
        Keyframe& kk = _project.keys[(size_t)single_selected()];
        if (ui::Checkbox(msg::key_hold, &kk.hold)) project_changed();
        ui::help_on_hover(msg::key_hold_help);
        if (ui::Checkbox(msg::key_aim, &kk.aim)) {
            if (kk.aim) {
                // Aim at what the camera was already looking at, a little way on.
                float c2w[12], tgt[3];
                _panel->nav_pose(c2w, tgt);
                double ahead[3];
                double R[9];
                quat_to_matrix3(kk.rot, R);
                const double d = 1.0 / std::max(_w2s.s, 1e-12);
                for (int a = 0; a < 3; a++) ahead[a] = kk.pos[a] - R[a*3+2] * d;
                for (int a = 0; a < 3; a++) kk.target[a] = ahead[a];
                kk.roll = roll_of(kk.rot, kk.pos, kk.target, _project.up);
                update_aim(kk, _project.up);
            }
            project_changed();
        }
        ui::help_on_hover(msg::key_aim_help);
        if (kk.aim) {
            ImGui::SameLine();
            if (ui::KeyButton(msg::key_pick_target, 0.0f, "T", _pick_target)) _pick_target = true;
            ui::help_on_hover(msg::key_pick_target_help);
            ImGui::SetNextItemWidth(w);
            float roll = (float)kk.roll;
            if (ui::SliderFloatRaw("##roll", &roll, -180.0f, 180.0f, "%.1f\xc2\xb0")) {
                kk.roll = roll;
                update_aim(kk, _project.up);
                project_changed();
            }
            ImGui::SameLine();
            ui::Text(msg::key_roll);
            float tg[3] = {(float)kk.target[0], (float)kk.target[1], (float)kk.target[2]};
            ImGui::SetNextItemWidth(w);
            if (ui::DragFloat3Raw("##target", tg, 0.01f / (float)std::max(_w2s.s, 1e-9), "%.3f")) {
                for (int a = 0; a < 3; a++) kk.target[a] = tg[a];
                update_aim(kk, _project.up);
                project_changed();
            }
            ImGui::SameLine();
            ui::Text(msg::key_target);
        }
        float p[3] = {(float)kk.pos[0], (float)kk.pos[1], (float)kk.pos[2]};
        ImGui::SetNextItemWidth(w);
        if (ui::DragFloat3Raw("##pos", p, 0.01f / (float)std::max(_w2s.s, 1e-9), "%.3f")) {
            for (int a = 0; a < 3; a++) kk.pos[a] = p[a];
            update_aim(kk, _project.up);
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::key_position);
    } else if (any) {
        ui::TextDisabledWrapped(msg::keys_many_hint);
    }

    if (n >= 2) {
        static float total = 0.0f;
        if (total <= 0.0f) total = (float)_project.duration();
        ImGui::SetNextItemWidth(full * 0.3f);
        ui::InputFloatRaw("##total", &total, "%.1f s");
        ImGui::SameLine();
        if (ui::Button(msg::keys_space_evenly)) space_evenly(total);
        ui::help_on_hover(msg::keys_space_evenly_help);
    }
}

void RenderSession::draw_lens_section(float full) {
    if (_project.keys.empty()) return;
    int at = single_selected();
    if (at < 0) {
        // No one key chosen: the lens in force at the playhead.
        at = 0;
        for (int i = 0; i < (int)_project.keys.size(); i++)
            if (_project.keys[(size_t)i].time <= _time + 1e-9) at = i;
    }
    Keyframe& k = _project.keys[(size_t)at];
    ui::TextDisabled(msg::lens_of_key, {(long long)(at + 1)});
    if (at > 0) {
        bool same = !k.own_lens;
        if (ui::Checkbox(msg::lens_same_as_before, &same)) {
            k.own_lens = !same;
            if (k.own_lens) k.lens = _project.lens_at(at - 1);
            project_changed();
        }
        ui::help_on_hover(msg::lens_same_as_before_help);
        if (same) {
            const Lens& l = _project.lens_at(at);
            ui::TextDisabled(msg::lens_inherited,
                             {kProjections[(int)l.projection]->get(), (long long)std::lround(lens_fov(l))});
            return;
        }
    }
    Lens& l = k.lens;
    const float w = full * 0.62f;
    const bool first_key = at == 0;
    (void)first_key;

    int proj = (int)l.projection;
    ImGui::SetNextItemWidth(w);
    if (combo_msgs("##proj", &proj, kProjections, kNumProjections)) {
        const double fov = lens_fov(l);
        l.projection = (Projection)proj;
        if (l.projection == Projection::Equirect) {
            l.tier = 0;
            for (float& d : l.dist) d = 0.0f;
            // The whole sphere wants a 2:1 frame.
            if (_project.output.width != 2 * _project.output.height) {
                _project.output.height = std::max(16, _project.output.width / 2);
            }
        } else {
            lens_set_fov(l, l.projection == Projection::Perspective ? std::min(fov, 120.0)
                                                                    : std::max(fov, 120.0));
        }
        project_changed();
    }
    ImGui::SameLine();
    ui::Text(msg::lens_projection);
    ui::help_on_hover(msg::lens_projection_help);

    // Presets: what a photographer names, what a 360 camera makes, and what
    // this dataset was shot with.
    ImGui::SetNextItemWidth(w);
    if (ui::BeginCombo(msg::lens_preset, msg::lens_preset_pick.get(), ImGuiComboFlags_HeightLarge)) {
        auto apply = [&](const Lens& lens, int pw, int ph) {
            l = lens;
            if (pw > 0 && ph > 0) {
                _project.output.width = pw;
                _project.output.height = ph;
            }
            project_changed();
        };
        ui::SeparatorText(msg::lens_presets_common);
        int id = 0;
        for (const LensPreset& p : generic_lens_presets()) {
            ImGui::PushID(id++);
            if (ui::SelectableRaw(preset_label(p))) apply(p.lens, p.width, p.height);
            ImGui::PopID();
        }
        ui::SeparatorText(msg::lens_presets_360);
        for (const LensPreset& p : camera_lens_presets()) {
            ImGui::PushID(id++);
            if (ui::SelectableRaw(preset_label(p))) apply(p.lens, p.width, p.height);
            ImGui::PopID();
        }
        // The dataset's own lenses, clustered once per dataset.
        const ParsedDataset* ds = nullptr;
        for (const SourceInfo& s : _sources) {
            if (s.view.kind == SourceView::Points && s.view.ds) { ds = s.view.ds; break; }
            if (s.dataset) { ds = s.dataset; break; }
        }
        if (ds && ds->num_cameras > 0) {
            const std::string key = std::to_string((uintptr_t)ds) + ":" +
                                    std::to_string(ds->num_cameras);
            if (key != _dataset_lenses_key) {
                _dataset_lenses = cluster_dataset_lenses(*ds);
                _dataset_lenses_key = key;
            }
            ui::SeparatorText(msg::lens_presets_dataset);
            for (const DatasetLens& d : _dataset_lenses) {
                char b[160];
                std::snprintf(b, sizeof b, "%s  %dx%d  %.0f\xc2\xb0",
                              kProjections[(int)d.lens.projection]->get(), d.width,
                              d.height, lens_fov(d.lens));
                ImGui::PushID(id++);
                const std::string label = format(msg::lens_dataset_entry, {std::string(b), (long long)d.count});
                if (ui::SelectableRaw(label)) apply(d.lens, d.width, d.height);
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }

    if (l.projection != Projection::Equirect) {
        float fov = (float)lens_fov(l);
        const float lo = 5.0f, hi = l.projection == Projection::Perspective ? 150.0f : 360.0f;
        ImGui::SetNextItemWidth(w);
        if (ui::SliderFloatRaw("##fov", &fov, lo, hi, "%.1f\xc2\xb0")) {
            lens_set_fov(l, fov);
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::lens_fov);
        ui::help_on_hover(msg::lens_fov_help);
        if (l.projection == Projection::Perspective) {
            float mm = (float)lens_mm(l);
            ImGui::SetNextItemWidth(w);
            if (ui::InputFloatRaw("##mm", &mm, 1.0f, 10.0f, "%.1f mm")) {
                l.focal = std::clamp((double)mm, 2.0, 2000.0) / 36.0;
                project_changed();
            }
            ImGui::SameLine();
            ui::Text(msg::lens_mm);
            ui::help_on_hover(msg::lens_mm_help);
        }
        // k1 alone covers most of what a lens does; the rest is Advanced.
        float k1 = l.tier ? l.dist[0] : 0.0f;
        ImGui::SetNextItemWidth(w);
        if (ui::SliderFloatRaw("##k1", &k1, -0.5f, 0.5f, "k1 %.4f")) {
            if (!l.tier) l.tier = 1;
            l.dist[0] = k1;
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::lens_distortion);
        ui::help_on_hover(msg::lens_distortion_help);
        if (ui::TreeNode(msg::lens_all_coefficients)) {
            int tier = l.tier;
            ImGui::SetNextItemWidth(w);
            if (ui::ComboRaw("##tier", &tier, {&msg::tier_none, &msg::tier_opencv,
                                               &msg::tier_full})) {
                l.tier = tier;
                if (!tier) for (float& d : l.dist) d = 0.0f;
                project_changed();
            }
            static const char* const kNames[2][8] = {
                {"k1", "k2", "p1", "p2", "", "", "", ""},
                {"k1", "k2", "k3", "k4", "p1", "p2", "s1", "s2"}};
            const int n = l.tier == 1 ? 4 : l.tier == 2 ? 8 : 0;
            for (int i = 0; i < n; i++) {
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(w);
                if (ui::InputFloatRaw("##c", &l.dist[i], "%.6f")) project_changed();
                ImGui::SameLine();
                ui::TextRaw(kNames[l.tier - 1][i]);
                ImGui::PopID();
            }
            if (n) {
                // Paste a whole row -- COLMAP's order for the full tier.
                static std::string paste;
                ImGui::SetNextItemWidth(w);
                if (ui::InputTextWithHintRaw("##paste", msg::lens_paste_hint, &paste,
                                             ImGuiInputTextFlags_EnterReturnsTrue)) {
                    float v[8] = {};
                    int got = 0;
                    const char* s = paste.c_str();
                    while (got < 8 && *s) {
                        char* end = nullptr;
                        const float f = std::strtof(s, &end);
                        if (end == s) { s++; continue; }
                        v[got++] = f;
                        s = end;
                    }
                    if (got >= n) {
                        if (l.tier == 2 && got == 8) {
                            // COLMAP THIN_PRISM_FISHEYE: k1 k2 p1 p2 k3 k4 sx1 sy1.
                            const float r[8] = {v[0], v[1], v[4], v[5], v[2], v[3], v[6], v[7]};
                            for (int i = 0; i < 8; i++) l.dist[i] = r[i];
                        } else {
                            for (int i = 0; i < n; i++) l.dist[i] = v[i];
                        }
                        project_changed();
                    }
                    paste.clear();
                }
                ui::help_on_hover(msg::lens_paste_help);
            }
            ImGui::TreePop();
        }
    } else {
        ui::TextDisabled(msg::lens_equirect_note);
    }
}

void RenderSession::draw_motion_section(float full) {
    Motion& m = _project.motion;
    if (ui::Checkbox(msg::motion_smooth, &m.smooth)) project_changed();
    ui::help_on_hover(msg::motion_smooth_help);
    if (ui::Checkbox(msg::motion_ease, &m.ease)) project_changed();
    ui::help_on_hover(msg::motion_ease_help);
    if (ui::Checkbox(msg::motion_constant, &m.constant_speed)) project_changed();
    ui::help_on_hover(msg::motion_constant_help);
    if (m.smooth) {
        float t = (float)m.tension;
        ImGui::SetNextItemWidth(full * 0.5f);
        if (ui::SliderFloatRaw("##tension", &t, 0.0f, 1.0f, "%.2f")) {
            m.tension = t;
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::motion_tension);
        ui::help_on_hover(msg::motion_tension_help);
    }
    if (ui::Button(msg::up_from_view)) take_up_from_view();
    ui::help_on_hover(msg::up_from_view_help);
    char len[32];
    std::snprintf(len, sizeof len, "%.3g", trajectory().length());
    ui::TextDisabled(msg::motion_length, {std::string(len), seconds(_project.duration())});
}

void RenderSession::draw_effects_section(float full) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = full * 0.5f;

    // Fades: the two every editor has.
    auto fade = [&](const Msg& name, Fade& f, const char* id) {
        ImGui::PushID(id);
        int c = (int)f.colour;
        ImGui::SetNextItemWidth(w * 0.7f);
        if (ui::ComboRaw("##c", &c, {&msg::fade_none, &msg::fade_black, &msg::fade_white})) {
            f.colour = (FadeColour)c;
            project_changed();
        }
        ImGui::SameLine();
        if (f.colour != FadeColour::None) {
            float s = (float)f.seconds;
            ImGui::SetNextItemWidth(w * 0.55f);
            if (ui::SliderFloatRaw("##s", &s, 0.1f, 5.0f, "%.1f s")) {
                f.seconds = s;
                project_changed();
            }
            ImGui::SameLine();
        }
        ui::Text(name);
        ImGui::PopID();
    };
    fade(msg::fade_in, _project.fade_in, "fi");
    fade(msg::fade_out, _project.fade_out, "fo");
    if (ui::ColorEdit3Raw("##bg", _project.background, ImGuiColorEditFlags_NoInputs))
        project_changed();
    ImGui::SameLine();
    ui::Text(msg::background);

    // The models, and how each is drawn.
    ui::SeparatorText(msg::sec_models);
    for (int i = 0; i < (int)_sources.size(); i++) {
        const SourceInfo& s = _sources[(size_t)i];
        if (i >= (int)_project.sources.size()) break;
        SourceStyle& y = _project.sources[(size_t)i].style;
        ImGui::PushID(i);
        const Msg& kind = s.view.kind == SourceView::Points ? msg::model_points
                          : s.view.kind == SourceView::Mesh ? msg::model_mesh
                                                            : msg::model_splats;
        ui::TextRaw(format(msg::model_line, {(long long)(i + 1), kind.get(), s.name}));
        if (ImGui::IsItemHovered()) ui::SetTooltipRaw(s.path);
        ImGui::Indent();
        if (s.view.kind == SourceView::Points) {
            int ps = (int)y.point_style;
            ImGui::SetNextItemWidth(w);
            if (combo_msgs("##ps", &ps, kPointStyles, kNumPointStyles)) {
                y.point_style = (PointStyle)ps;
                project_changed();
            }
            ImGui::SameLine();
            ui::Text(msg::point_style);
            ImGui::SetNextItemWidth(w);
            if (y.point_style == PointStyle::Sphere) {
                if (ui::SliderFloatRaw("##pr", &y.sphere_radius, 0.0005f, 0.05f, "%.4f",
                                       ImGuiSliderFlags_Logarithmic))
                    project_changed();
            } else if (ui::SliderFloatRaw("##px", &y.point_px, 1.0f, 24.0f, "%.1f px")) {
                project_changed();
            }
            ImGui::SameLine();
            ui::Text(msg::point_size);
            ui::help_on_hover(msg::point_size_help);
            if (ui::Checkbox(msg::point_cameras, &y.cameras)) project_changed();
        } else if (s.view.kind == SourceView::Mesh) {
            if (ui::Checkbox(msg::mesh_shade, &y.shade)) project_changed();
            ImGui::SameLine();
            if (ui::Checkbox(msg::mesh_flat, &y.flat)) project_changed();
            ImGui::SameLine();
            if (ui::Checkbox(msg::mesh_colour, &y.colour)) project_changed();
        } else if (s.view.sh_max > 0) {
            int sh = y.sh_degree < 0 ? s.view.sh_max : y.sh_degree;
            ImGui::SetNextItemWidth(w);
            if (ui::SliderIntRaw("##sh", &sh, 0, s.view.sh_max, "SH %d")) {
                y.sh_degree = sh == s.view.sh_max ? -1 : sh;
                project_changed();
            }
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (ui::Button(msg::model_add) && _pick)
        _pick(Pick::AddModel, _sources.empty() ? std::string() : _sources[0].path, "");
    ui::help_on_hover(msg::model_add_help);

    // Shots: which model is shown from when, and how it arrives.
    ui::SeparatorText(msg::sec_shots);
    int remove = -1;
    for (int i = 0; i < (int)_project.shots.size(); i++) {
        Shot& s = _project.shots[(size_t)i];
        ImGui::PushID(1000 + i);
        float start = (float)s.start;
        ImGui::SetNextItemWidth(px(70.0f));
        if (ui::DragFloatRaw("##start", &start, 0.05f, 0.0f, 3600.0f, "%.2f s")) {
            s.start = std::max(0.0f, start);
            project_changed();
        }
        ImGui::SameLine();
        std::vector<std::string> names;
        names.push_back(msg::model_nothing.get());
        for (const SourceInfo& src : _sources) names.push_back(src.name);
        int src = s.source + 1;
        ImGui::SetNextItemWidth(full - px(70.0f) - px(28.0f) - 3.0f * st.ItemSpacing.x - w * 0.9f);
        if (ui::BeginComboRaw("##src", names[(size_t)std::clamp(src, 0, (int)names.size() - 1)].c_str())) {
            for (int j = 0; j < (int)names.size(); j++)
                if (ui::SelectableRaw(names[(size_t)j], j == src)) {
                    s.source = j - 1;
                    project_changed();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        int tr = (int)s.transition;
        ImGui::SetNextItemWidth(w * 0.9f);
        if (combo_msgs("##tr", &tr, kTransitions, kNumTransitions)) {
            s.transition = (Transition)tr;
            project_changed();
        }
        ImGui::SameLine();
        if (ui::ButtonRaw("x##del", ImVec2(px(28.0f), 0))) remove = i;
        if (s.transition != Transition::Cut) {
            float d = (float)s.duration;
            ImGui::SetNextItemWidth(px(120.0f));
            if (ui::SliderFloatRaw("##dur", &d, 0.1f, 6.0f, "%.1f s")) {
                s.duration = d;
                project_changed();
            }
            ImGui::SameLine();
            ui::TextDisabled(msg::shot_transition_time);
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        _project.shots.erase(_project.shots.begin() + remove);
        project_changed();
    }
    if (ui::Button(msg::shot_add)) {
        Shot s;
        s.start = _time;
        s.source = _project.shots.empty() ? 0 : _project.shots.back().source;
        s.transition = Transition::Crossfade;
        if (_project.shots.empty() && _time > 0.0) _project.shots.push_back(Shot{});
        _project.shots.push_back(s);
        std::stable_sort(_project.shots.begin(), _project.shots.end(),
                         [](const Shot& a, const Shot& b) { return a.start < b.start; });
        project_changed();
    }
    ui::help_on_hover(msg::shot_add_help);

    // The one-button story: the cameras' points, the splats growing out of
    // them, the surface sweeping up over both.
    int points = -1, splats = -1, mesh = -1;
    for (int i = 0; i < (int)_sources.size(); i++) {
        const SourceView::Kind k = _sources[(size_t)i].view.kind;
        if (k == SourceView::Points && points < 0) points = i;
        if (k == SourceView::Splats && splats < 0) splats = i;
        if (k == SourceView::Mesh && mesh < 0) mesh = i;
    }
    const bool can_story = splats >= 0 && (points >= 0 || mesh >= 0);
    ImGui::BeginDisabled(!can_story || _project.duration() <= 0.0);
    if (ui::Button(msg::story_button, ImVec2(full, 0))) {
        const double T = _project.duration();
        _project.shots.clear();
        if (points >= 0) {
            _project.shots.push_back({0.0, points, Transition::Crossfade, 0.8});
            _project.shots.push_back({T * (mesh >= 0 ? 0.22 : 0.35), splats,
                                      Transition::Grow, T * (mesh >= 0 ? 0.25 : 0.35)});
        } else {
            _project.shots.push_back({0.0, splats, Transition::Crossfade, 0.8});
        }
        if (mesh >= 0)
            _project.shots.push_back({T * 0.62, mesh, Transition::Sweep, T * 0.22});
        project_changed();
    }
    ImGui::EndDisabled();
    ui::help_on_hover_disabled(can_story ? msg::story_help : msg::story_needs);
}

void RenderSession::draw_project_section(float full) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float third = (full - 2.0f * st.ItemSpacing.x) / 3.0f;
    const std::string dir = default_project_dir(_sources.empty() ? "" : _sources[0].path);
    if (ui::KeyButton(msg::project_save, third, "Ctrl+S")) {
        if (_project_path.empty()) {
            if (_pick) _pick(Pick::SaveProject, dir, "camera.json");
        } else {
            save_to(_project_path);
        }
    }
    ImGui::SameLine();
    if (ui::Button(msg::project_save_as, ImVec2(third, 0)) && _pick)
        _pick(Pick::SaveProject, _project_path.empty() ? dir
                                 : fs::u8path(_project_path).parent_path().string(),
              _project_path.empty() ? "camera.json"
                                    : fs::u8path(_project_path).filename().string());
    ImGui::SameLine();
    if (ui::Button(msg::project_open, ImVec2(third, 0)) && _pick)
        _pick(Pick::OpenProject, dir, "");
    if (!_project_path.empty()) {
        ui::TextDisabledRaw(elide_middle(_project_path, full));
        if (ImGui::IsItemHovered()) ui::SetTooltipRaw(_project_path);
    } else {
        ui::TextDisabledWrapped(msg::project_where, {dir});
    }
    // What is already there, one click away.
    std::error_code ec;
    if (fs::is_directory(fs::u8path(dir), ec)) {
        int shown = 0;
        for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec)) {
            if (e.path().extension() != ".json" || shown >= 8) continue;
            ImGui::PushID(shown++);
            if (ui::SelectableRaw(e.path().filename().string())) open_from(e.path().string());
            ImGui::PopID();
        }
    }

    // The whole move at once, for a model turned in the editor after the
    // path was laid out.
    ui::SeparatorText(msg::sec_whole_path);
    ui::TextDisabledWrapped(msg::whole_path_hint);
    const char* axes[3] = {"X", "Y", "Z"};
    for (int a = 0; a < 3; a++) {
        ImGui::PushID(a);
        if (a) ImGui::SameLine();
        if (ui::Button(msg::whole_path_turn, {std::string(axes[a])})) {
            double axis[3] = {0, 0, 0};
            axis[a] = 1.0;
            double c[3] = {0, 0, 0};
            for (const Keyframe& k : _project.keys)
                for (int d = 0; d < 3; d++) c[d] += k.pos[d] / (double)_project.keys.size();
            transform_project(_project, spirula::Sim3::rotation_about(axis, kPi / 2, c));
            project_changed();
        }
        ImGui::PopID();
    }
}

void RenderSession::draw_output_section(float full) {
    Output& o = _project.output;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = full * 0.62f;
    ImGui::BeginDisabled(exporting());

    // Size: named first, numbers for anyone who has their own.
    char cur[64];
    std::snprintf(cur, sizeof cur, "%d \xc3\x97 %d", o.width, o.height);
    ImGui::SetNextItemWidth(w);
    if (ui::BeginComboRaw("##res", cur)) {
        for (int i = 0; i < kNumResolutions; i++) {
            const Resolution& r = kResolutions[i];
            char b[96];
            std::snprintf(b, sizeof b, "%d \xc3\x97 %d", r.w, r.h);
            ImGui::PushID(i);
            if (ui::SelectableRaw(format(msg::res_entry, {r.name->get(), std::string(b)}),
                                  o.width == r.w && o.height == r.h)) {
                o.width = r.w;
                o.height = r.h;
                project_changed();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ui::Text(msg::out_size);
    int wh[2] = {o.width, o.height};
    ImGui::SetNextItemWidth(w);
    if (ui::InputInt2Raw("##wh", wh, ImGuiInputTextFlags_EnterReturnsTrue)) {
        o.width = std::clamp(wh[0], 16, 16384);
        o.height = std::clamp(wh[1], 16, 16384);
        project_changed();
    }
    ImGui::SameLine();
    ui::TextDisabled(msg::out_pixels);

    if (o.kind == OutputKind::Video || o.kind == OutputKind::Frames) {
        static const double kRates[] = {24, 25, 30, 50, 60};
        char r[32];
        std::snprintf(r, sizeof r, "%.3g fps", o.fps);
        ImGui::SetNextItemWidth(w);
        if (ui::BeginComboRaw("##fps", r)) {
            for (double f : kRates) {
                char b[32];
                std::snprintf(b, sizeof b, "%.3g fps", f);
                if (ui::SelectableRaw(b, o.fps == f)) { o.fps = f; project_changed(); }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ui::Text(msg::out_fps);
    }
    if (o.kind == OutputKind::Video) {
        int codec = (int)o.codec;
        ImGui::SetNextItemWidth(w);
        if (ui::ComboRaw("##codec", &codec, {&msg::codec_h264, &msg::codec_h265})) {
            o.codec = (Codec)codec;
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::out_codec);
        ui::help_on_hover(msg::out_codec_help);
        ImGui::SetNextItemWidth(w);
        if (ui::ComboRaw("##q", &o.quality, {&msg::quality_best, &msg::quality_standard,
                                             &msg::quality_small}))
            project_changed();
        ImGui::SameLine();
        ui::Text(msg::out_quality);
        const int probe = _encoder_probe.load();
        const Msg& enc = builtin_encodes(o.codec == Codec::H265) ? msg::encoder_builtin
                         : probe == 1 ? msg::encoder_checking
                         : command_exists(_ffmpeg) ? msg::encoder_ffmpeg : msg::encoder_none;
        ui::TextDisabledWrapped(enc);
    } else {
        int f = (int)o.format;
        ImGui::SetNextItemWidth(w);
        if (ui::ComboRaw("##fmt", &f, {&msg::format_png, &msg::format_jpeg})) {
            o.format = (ImageFormat)f;
            fit_output_path(o);
            project_changed();
        }
        ImGui::SameLine();
        ui::Text(msg::out_format);
        if (o.format == ImageFormat::Jpeg) {
            ImGui::SetNextItemWidth(w);
            if (ui::SliderIntRaw("##jq", &o.jpeg_quality, 50, 100, "%d")) project_changed();
            ImGui::SameLine();
            ui::Text(msg::out_quality);
        } else if (ui::Checkbox(msg::out_transparent, &o.transparent)) {
            project_changed();
        }
    }

    // Where it goes. A video and a photo are files; frames go in a folder.
    ImGui::SetNextItemWidth(full - px(90.0f) - st.ItemSpacing.x);
    std::string path = o.path;
    if (ui::InputTextWithHintRaw("##out", msg::out_path_hint, &path,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        o.path = path;
        project_changed();
    }
    ImGui::SameLine();
    if (ui::Button(msg::out_browse, ImVec2(px(90.0f), 0)) && _pick)
        _pick(Pick::Output, _sources.empty() ? "" : default_project_dir(_sources[0].path), "");
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (!exporting()) {
        const Msg& go = o.kind == OutputKind::Photo ? msg::render_photo
                        : o.kind == OutputKind::Video ? msg::render_video : msg::render_frames;
        ImGui::BeginDisabled(_project.keys.empty());
        if (ui::Button(go, ImVec2(full, ImGui::GetFrameHeight() * 1.6f))) start_export();
        ImGui::EndDisabled();
        const double T = _project.duration();
        if (o.kind != OutputKind::Photo)
            ui::TextDisabled(msg::out_summary, {seconds(T),
                             (long long)std::max(1L, std::lround(T * o.fps) + 1)});
    } else {
        const float frac = _job.frames > 0 ? (float)_job.frame / (float)_job.frames : 0.0f;
        ui::ProgressBar(frac, ImVec2(full, 0), msg::render_progress,
                        {(long long)_job.frame, (long long)_job.frames});
        if (ui::Button(msg::render_cancel, ImVec2(full, 0))) cancel_export();
    }
}

}  // namespace gui::render
