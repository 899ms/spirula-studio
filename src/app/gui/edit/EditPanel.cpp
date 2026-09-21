// EditPanel.cpp -- the editing panel and the keys that drive it, for the
// session in EditSession.h.

#include "app/gui/edit/EditSession.h"

#include "app/gui/Ui.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>

namespace msg = spirula::i18n::msg::edit;
using spirula::i18n::Msg;

namespace gui {

namespace {

// Everything the panel offers besides the tools, with the key it answers to:
// one table, so the letter on a button and the letter the handler listens for
// cannot drift. A key is an identifier, so it never gets translated.
enum class Act {
    All, None, Invert, Grow, Shrink, Floaters,
    Delete, Isolate, Restore, Undo, Redo,
    Replace, Add, Subtract, Intersect
};

struct ActRow {
    Act act;
    const char* key;
    ImGuiKey imgui_key;
    bool shift, ctrl, alt;
    // Whether the key is also one of NavCamera's fly keys (WASDQE): while
    // Navigate is the active tool those belong to the camera.
    bool fly;
};

const ActRow kActs[] = {
    {Act::All,       "A",      ImGuiKey_A,     false, false, false, true},
    {Act::None,      "Alt+A",  ImGuiKey_A,     false, false, true,  true},
    {Act::Invert,    "I",      ImGuiKey_I,     false, false, false, false},
    {Act::Grow,      "+",      ImGuiKey_Equal, false, false, false, false},
    {Act::Shrink,    "-",      ImGuiKey_Minus, false, false, false, false},
    {Act::Floaters,  "Shift+F",ImGuiKey_F,     true,  false, false, false},
    {Act::Delete,    "X",      ImGuiKey_X,     false, false, false, false},
    {Act::Isolate,   "Shift+X",ImGuiKey_X,     true,  false, false, false},
    {Act::Restore,   "Alt+X",  ImGuiKey_X,     false, false, true,  false},
    {Act::Undo,      "Ctrl+Z", ImGuiKey_Z,     false, true,  false, false},
    {Act::Redo,      "Ctrl+Y", ImGuiKey_Y,     false, true,  false, false},
    {Act::Replace,   "1",      ImGuiKey_1,     false, false, false, false},
    {Act::Add,       "2",      ImGuiKey_2,     false, false, false, false},
    {Act::Subtract,  "3",      ImGuiKey_3,     false, false, false, false},
    {Act::Intersect, "4",      ImGuiKey_4,     false, false, false, false},
    // Aliases: what someone arriving from another editor reaches for. Later
    // than the primaries, so the key printed on a button is still the first.
    {Act::All,       "Ctrl+A", ImGuiKey_A,     false, true,  false, false},
    {Act::None,      "Ctrl+D", ImGuiKey_D,     false, true,  false, false},
    {Act::Invert,    "Ctrl+I", ImGuiKey_I,     false, true,  false, false},
    {Act::Delete,    "Del",    ImGuiKey_Delete,false, false, false, false},
    {Act::Redo,      "Ctrl+Shift+Z", ImGuiKey_Z, true, true, false, false},
};
constexpr int kNumActs = (int)(sizeof kActs / sizeof kActs[0]);

const ActRow& act_row(Act a) {
    for (const ActRow& r : kActs)
        if (r.act == a) return r;
    return kActs[0];
}

// Down-and-up this frame, with exactly the modifiers the row asks for.
bool act_pressed(const ActRow& r, bool fly_keys_taken) {
    if (r.fly && fly_keys_taken) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift != r.shift || io.KeyCtrl != r.ctrl || io.KeyAlt != r.alt)
        return false;
    return ImGui::IsKeyPressed(r.imgui_key, false);
}

// The key in the corner of the button it belongs to: a modal grammar nobody
// can see is a modal grammar nobody uses.
void draw_corner_key(const char* key) {
    if (!key || !*key) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float tw = ImGui::CalcTextSize(key).x;
    if (tw + 2.0f * st.FramePadding.x > b.x - a.x) return;   // no room
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(b.x - st.FramePadding.x - tw, a.y + st.FramePadding.y),
        IM_COL32(255, 255, 255, 90), key);
}

bool key_button(const Msg& m, float w, const char* key, bool on = false) {
    if (on) {
        const ImVec4 c = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    }
    const bool hit = ui::Button(m, ImVec2(w, 0));
    if (on) ImGui::PopStyleColor(2);
    draw_corner_key(key);
    return hit;
}

bool act_button(Act a, const Msg& m, float w) {
    return key_button(m, w, act_row(a).key);
}

// A checkbox whose change goes through the history. The widget writes a
// scratch copy so the setting itself only moves inside the op.
void option_box(EditSession& s, const Msg& m, bool* slot) {
    bool v = *slot;
    if (ui::Checkbox(m, &v)) s.set_option(slot, v, m);
}

// A slider the same way, committed ONCE when the drag ends -- one history
// entry per gesture rather than one per frame. The value it started from is
// what undo goes back to.
void option_slider(EditSession& s, const Msg& m, float* slot, float lo,
                   float hi, const char* fmt, float w) {
    static const void* active = nullptr;
    static float started_at = 0.0f;
    float v = *slot;
    ImGui::SetNextItemWidth(w);
    ui::SliderFloat(m, &v, lo, hi, fmt);
    if (ImGui::IsItemActivated()) {
        active = slot;
        started_at = *slot;
    }
    if (v != *slot) *slot = v;              // live, so the preview follows
    if (ImGui::IsItemDeactivatedAfterEdit() && active == slot) {
        const float now = *slot;
        *slot = started_at;
        s.set_number(slot, now, m);
        active = nullptr;
    }
}

void option_slider_int(EditSession& s, const Msg& m, int* slot, int lo, int hi,
                       float w) {
    static const void* active = nullptr;
    static int started_at = 0;
    int v = *slot;
    ImGui::SetNextItemWidth(w);
    ui::SliderInt(m, &v, lo, hi);
    if (ImGui::IsItemActivated()) {
        active = slot;
        started_at = *slot;
    }
    if (v != *slot) *slot = v;
    if (ImGui::IsItemDeactivatedAfterEdit() && active == slot) {
        const int now = *slot;
        *slot = started_at;
        s.set_number(slot, now, m);
        active = nullptr;
    }
}

}  // namespace


void EditSession::handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (!_doc) return;

    // While Navigate is the active tool the camera owns WASDQE, so the keys
    // that collide with it are not read here. Every other key still is, which
    // is how a letter switches away from Navigate in the first place.
    const bool fly = _tool.id() == ToolId::Navigate;

    for (int i = 0; i < kNumTools; i++) {
        const ToolRow& row = tool_table()[i];
        if (row.fly_key && fly) continue;
        if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
            ImGui::IsKeyPressed((ImGuiKey)row.imgui_key, false)) {
            _tool.set_id(row.id);
            // The key is still down this frame; the camera must not also read
            // it on the way into Navigate.
            if (row.fly_key) _fly_block_key = row.imgui_key;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) _tool.cancel();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        ShapeStroke s;
        ViewportInput mods;
        mods.shift = io.KeyShift;
        mods.ctrl = io.KeyCtrl;
        if (_tool.commit_pending(s)) apply_stroke(s, mods);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true))
        _tool.set_brush_radius(std::max(2.0f, _tool.brush_radius() * 0.85f));
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true))
        _tool.set_brush_radius(std::min(400.0f, _tool.brush_radius() * 1.18f));

    // While a polygon is still being drawn, Ctrl+Z belongs to the polygon:
    // taking the last corner back is what it obviously means there.
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) &&
        _tool.id() == ToolId::Polygon && _tool.in_progress() &&
        _tool.pop_point())
        return;

    for (const ActRow& r : kActs) {
        if (!act_pressed(r, fly)) continue;
        switch (r.act) {
            case Act::All:       select_all(true); break;
            case Act::None:      select_all(false); break;
            case Act::Invert:    invert_selection(); break;
            case Act::Grow:      grow_shrink(true); break;
            case Act::Shrink:    grow_shrink(false); break;
            case Act::Floaters:  keep_largest_components(); break;
            case Act::Delete:
                if (!_doc->sel().empty()) _doc->run(make_hide_op(*_doc, false));
                break;
            case Act::Isolate:
                if (!_doc->sel().empty()) _doc->run(make_hide_op(*_doc, true));
                break;
            case Act::Restore:
                _doc->run(make_reveal_op(*_doc));
                break;
            case Act::Undo:
                _doc->undo();
                break;
            case Act::Redo:
                _doc->redo();
                break;
            case Act::Replace:   _combine = (int)Combine::Replace; break;
            case Act::Add:       _combine = (int)Combine::Add; break;
            case Act::Subtract:  _combine = (int)Combine::Subtract; break;
            case Act::Intersect: _combine = (int)Combine::Intersect; break;
        }
        break;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) _ask_overwrite = true;
}


void EditSession::draw_status() {
    if (!_doc) return;
    if (busy()) {
        ui::Text(msg::working);
        return;
    }
    ui::TextDisabled(_tool.hint());
    if (_tool.owns_pointer()) {
        ImGui::SameLine();
        ui::TextDisabled(msg::hint_nav_with_tool);
    }
}


void EditSession::draw_panel() {
    if (!_doc) return;
    EditDoc& d = *_doc;
    const float full = ImGui::GetContentRegionAvail().x;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float half = (full - st.ItemSpacing.x) * 0.5f;
    const float third = (full - st.ItemSpacing.x * 2) / 3.0f;

    // Every slider gets the same width: ImGui puts the label to the right, so
    // sizing each to its own label leaves a ragged column of numbers.
    float label_w = 0.0f;
    for (const Msg* m : {&msg::opt_brush_size, &msg::opt_depth_near,
                         &msg::opt_depth_far, &msg::act_reach,
                         &msg::act_pieces_kept, &msg::opt_front_tol,
                         &msg::opt_extent_scale})
        label_w = std::max(label_w, ImGui::CalcTextSize(m->get()).x);
    const float slider_w =
        std::max(full - label_w - st.ItemInnerSpacing.x, full * 0.3f);

    // The body scrolls; the strip under it does not, and its height is
    // reserved whether or not anything is in it. A banner that appears at the
    // top of a panel moves every control out from under the cursor.
    const float strip = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##editbody", ImVec2(0, -strip));

    // A long walk over the elements is in flight and every action below
    // depends on what it finds.
    ImGui::BeginDisabled(busy());

    // ---- what a tool works on ----
    if (d.layer_count() > 1) {
        for (int i = 0; i < d.layer_count(); i++) {
            if (i) ImGui::SameLine();
            if (ui::RadioButton(d.layer_name(i), d.layer() == i)) set_layer(i);
        }
        ui::help_on_hover(msg::layer_help);
    }

    // ---- tools ----
    ui::SeparatorText(msg::sec_tool);
    {
        constexpr int kPerRow = 3;
        const float w = (full - st.ItemSpacing.x * (kPerRow - 1)) / kPerRow;
        int col = 0;
        for (int i = 0; i < kNumTools; i++) {
            if (col) ImGui::SameLine();
            const ToolRow& row = tool_table()[i];
            if (key_button(tool_label(row.id), w, row.key, _tool.id() == row.id))
                _tool.set_id(row.id);
            ui::help_on_hover(tool_hint(row.id));
            if (++col == kPerRow) col = 0;
        }
    }
    if (_tool.id() == ToolId::Brush) {
        float r = _tool.brush_radius();
        ImGui::SetNextItemWidth(slider_w);
        if (ui::SliderFloat(msg::opt_brush_size, &r, 2.0f, 300.0f, "%.0f"))
            _tool.set_brush_radius(r);
    }

    // ---- the set ----
    ui::SeparatorText(msg::sec_select);
    ui::Text(msg::stat_selected, {(long long)d.sel().count()});
    ImGui::SameLine();
    ui::TextDisabled(d.element_name());
    if (act_button(Act::All, msg::act_all, third)) select_all(true);
    ImGui::SameLine();
    if (act_button(Act::None, msg::act_none, third)) select_all(false);
    ImGui::SameLine();
    if (act_button(Act::Invert, msg::act_invert, third)) invert_selection();

    // What a new selection does to the one already there. The modifiers do
    // the same thing, which is what the tooltip says rather than a mode.
    {
        const Msg* labels[kNumCombine] = {&msg::combine_replace, &msg::combine_add,
                                          &msg::combine_subtract,
                                          &msg::combine_intersect};
        const Act acts[kNumCombine] = {Act::Replace, Act::Add, Act::Subtract,
                                       Act::Intersect};
        // Packed greedily: four of these do not fit on one line in a narrow
        // panel, and the fourth going off the edge is how it used to look.
        float x = 0.0f;
        for (int i = 0; i < kNumCombine; i++) {
            const char* key = act_row(acts[i]).key;
            const float w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                            ImGui::CalcTextSize(labels[i]->get()).x +
                            st.ItemInnerSpacing.x +
                            ImGui::CalcTextSize(key).x;
            if (i && x + st.ItemSpacing.x + w <= full) {
                ImGui::SameLine();
                x += st.ItemSpacing.x + w;
            } else {
                x = w;
            }
            if (ui::RadioButton(*labels[i], _combine == i)) _combine = i;
            ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
            ui::TextDisabledRaw(key);
        }
        ui::help_on_hover(msg::combine_help);
    }

    option_box(*this, msg::opt_front_only, &_opt.front_only);
    ui::help_on_hover(msg::opt_front_only_help);
    if (d.kind() == EditDoc::Kind::Splats) {
        option_box(*this, msg::opt_by_extent, &_opt.by_extent);
        ui::help_on_hover(msg::opt_by_extent_help);
    }
    option_box(*this, msg::opt_depth_limit, &_opt.depth_limit);
    ui::help_on_hover(msg::opt_depth_limit_help);
    if (_opt.depth_limit) {
        option_slider(*this, msg::opt_depth_near, &_opt.near_frac, 0.0f, 1.0f,
                      "%.2f", slider_w);
        option_slider(*this, msg::opt_depth_far, &_opt.far_frac, 0.0f, 1.0f,
                      "%.2f", slider_w);
    }

    if (act_button(Act::Grow, msg::act_grow, half)) grow_shrink(true);
    ImGui::SameLine();
    if (act_button(Act::Shrink, msg::act_shrink, half)) grow_shrink(false);
    if (act_button(Act::Floaters, msg::act_floaters, full))
        keep_largest_components();
    ui::help_on_hover(msg::act_floaters_help);

    if (ui::CollapsingHeader(msg::sec_advanced)) {
        option_slider(*this, msg::act_reach, &_radius_mul, 0.0f, 6.0f, "%.2f",
                      slider_w);
        ui::help_on_hover(msg::act_reach_help);
        option_slider_int(*this, msg::act_pieces_kept, &_keep_components, 1, 32,
                          slider_w);
        option_slider(*this, msg::opt_front_tol, &_opt.front_tol, 0.0f, 0.5f,
                      "%.3f", slider_w);
        ui::help_on_hover(msg::opt_front_tol_help);
        if (d.kind() == EditDoc::Kind::Splats)
            option_slider(*this, msg::opt_extent_scale, &_opt.extent_scale,
                          0.25f, 4.0f, "%.2f", slider_w);
    }

    // ---- what is done with it ----
    ui::SeparatorText(msg::sec_actions);
    ImGui::BeginDisabled(d.sel().empty());
    if (act_button(Act::Delete, msg::act_delete, half)) {
        d.run(make_hide_op(d, false));
    }
    ui::help_on_hover_disabled(d.sel().empty() ? msg::stat_nothing_selected
                                               : msg::act_delete_help);
    ImGui::SameLine();
    if (act_button(Act::Isolate, msg::act_isolate, half)) {
        d.run(make_hide_op(d, true));
    }
    ui::help_on_hover_disabled(d.sel().empty() ? msg::stat_nothing_selected
                                               : msg::act_isolate_help);
    ImGui::EndDisabled();
    const int64_t hidden = d.count() - d.alive_count();
    ImGui::BeginDisabled(hidden == 0);
    if (act_button(Act::Restore, msg::act_restore, full)) {
        d.run(make_reveal_op(d));
    }
    ImGui::EndDisabled();
    ui::Text(msg::stat_kept, {(long long)d.alive_count(), (long long)d.count()});
    if (hidden) ui::TextDisabled(msg::stat_hidden, {(long long)hidden});

    // ---- history ----
    ui::SeparatorText(msg::sec_history);
    ImGui::BeginDisabled(!d.can_undo());
    if (act_button(Act::Undo, msg::hist_undo, half)) {
        d.undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!d.can_redo());
    if (act_button(Act::Redo, msg::hist_redo, half)) {
        d.redo();
    }
    ImGui::EndDisabled();
    // Folded by default: at the default window width the save buttons are
    // below it, and the list is the part you go looking for.
    if (ui::CollapsingHeader(msg::sec_history)) {
        // Every step, so going back ten of them is one click rather than ten.
        const int n = (int)d.history().size();
        const float rows = (float)std::clamp(n + 1, 3, 8);
        ImGui::BeginChild("##hist", ImVec2(0, rows * ImGui::GetTextLineHeightWithSpacing()),
                          ImGuiChildFlags_Borders);
        int go_to = -1;
        ImGui::PushID("hist");
        for (int i = 0; i <= n; i++) {
            ImGui::PushID(i);
            const bool here = i == d.history_head();
            // Greyed past the current point: those steps are a redo away.
            const bool ahead = i > d.history_head();
            if (ahead) ImGui::PushStyleColor(ImGuiCol_Text,
                                             ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            const bool hit =
                i == 0 ? ui::Selectable(msg::hist_original, here)
                       : ui::SelectableRaw(d.history()[(size_t)i - 1]->label(),
                                           here);
            if (ahead) ImGui::PopStyleColor();
            if (hit) go_to = i;
            ImGui::PopID();
        }
        ImGui::PopID();
        ImGui::EndChild();
        if (go_to >= 0) {
            d.goto_step(go_to);
        }
    }

    // ---- saving ----
    ui::SeparatorText(msg::sec_save);
    const std::vector<SaveTarget> targets = d.save_targets();
    _save_target = std::clamp(_save_target, 0, (int)targets.size() - 1);
    if (targets.size() > 1) {
        std::vector<const Msg*> items;
        for (const SaveTarget& t : targets) items.push_back(t.label);
        ImGui::SetNextItemWidth(full);
        ui::ComboRaw("##savefmt", &_save_target, items);
    }
    const SaveTarget& target = targets[(size_t)_save_target];
    const std::string home = d.default_save_path(_save_target);
    ImGui::BeginDisabled(home.empty());
    if (key_button(msg::save_over, half, "Ctrl+S")) _ask_overwrite = true;
    ImGui::EndDisabled();
    if (!home.empty()) ui::help_on_hover(msg::save_over_help, {home});
    ImGui::SameLine();
    ImGui::BeginDisabled(target.folder || !_pick_save);
    if (ui::Button(msg::save_copy, ImVec2(half, 0))) ask_save_copy();
    ImGui::EndDisabled();
    if (d.kind() == EditDoc::Kind::Points && target.folder)
        ui::TextDisabledWrapped(msg::sparse_edit_help);
    if (d.dirty()) ui::TextDisabled(msg::save_unsaved);
    if (!_status.empty())
        ui::TextColoredWrappedRaw(_status_err ? ImVec4(1, 0.5f, 0.5f, 1)
                                              : ImVec4(0.6f, 0.9f, 0.6f, 1),
                                  _status);

    // Overwriting is the one action here that cannot be undone, so it is the
    // one that asks.
    if (_ask_overwrite) {
        _ask_overwrite = false;
        if (!home.empty()) ui::OpenPopup(msg::save_confirm_title);
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22.0f, 0.0f),
                             ImGuiCond_Appearing);
    if (ui::BeginPopupModal(msg::save_confirm_title)) {
        ui::TextWrapped(msg::save_confirm_body, {home});
        const int linked = d.linked_count();
        if (linked > 0) {
            // The other outputs of one run are the same surface; whether the
            // edit reaches them is the question this dialog is really for.
            ui::TextDisabledWrapped(msg::mesh_link_edits_help);
            if (ui::Button(msg::save_this_only)) {
                d.set_linked(false);
                save_in_place();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ui::Button(msg::save_all_files, {(long long)(linked + 1)})) {
                d.set_linked(true);
                save_in_place();
                ImGui::CloseCurrentPopup();
            }
        } else if (ui::Button(msg::save_over)) {
            save_in_place();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button(msg::discard_no)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
    ImGui::EndChild();

    if (busy()) {
        const int total = work_total(), done = work_done();
        // Only the search is cancellable: a half-written file is worse than
        // a wait for one that is finishing.
        const bool can_stop = !_save_busy.load();
        const float bw = can_stop
            ? ImGui::CalcTextSize(msg::cancel_job.get()).x +
                  2.0f * st.FramePadding.x + st.ItemSpacing.x
            : 0.0f;
        if (total > 1)
            ui::ProgressBar((float)done / (float)total, ImVec2(-bw, 0),
                            msg::saving_progress,
                            {(long long)done, (long long)total});
        else
            ui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-bw, 0),
                            msg::working);
        if (can_stop) {
            ImGui::SameLine();
            if (ui::Button(msg::cancel_job)) cancel_work();
        }
    }
}

}  // namespace gui
