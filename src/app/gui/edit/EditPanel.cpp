// EditPanel.cpp -- the editing panel and the keys that drive it, for the
// session in EditSession.h.

#include "app/gui/edit/EditSession.h"

#include "app/gui/Ui.h"
#include "i18n/catalog/Edit.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

namespace msg = spirula::i18n::msg::edit;
using spirula::i18n::Msg;

namespace gui {

namespace {

bool toggle_button(const Msg& m, bool on, float w, const char* key) {
    if (on) {
        const ImVec4 c = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    }
    const bool hit = ui::Button(m, ImVec2(w, 0));
    if (on) ImGui::PopStyleColor(2);
    // The key, in the corner of the button it belongs to: a modal grammar
    // nobody can see is a modal grammar nobody uses. It is an identifier, so
    // it is the same letter in every language.
    if (key && *key) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImGuiStyle& st = ImGui::GetStyle();
        const float tw = ImGui::CalcTextSize(key).x;
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(b.x - st.FramePadding.x - tw, a.y + st.FramePadding.y),
            IM_COL32(255, 255, 255, 90), key);
    }
    return hit;
}

// A slider in a narrow panel, with room left for its label -- ImGui draws the
// label to the RIGHT, so a full-width item leaves a column of bare numbers.
// Measured per language: "Reach" and "Радиус связи" are not the same width.
void slider_item_width(const Msg& m, float full) {
    const float tw = ImGui::CalcTextSize(m.get()).x +
                     ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(full - tw, full * 0.35f));
}

}  // namespace


void EditSession::handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (!_doc) return;

    for (int i = 0; i < kNumTools; i++) {
        const ToolRow& row = tool_table()[i];
        if (!io.KeyCtrl && ImGui::IsKeyPressed((ImGuiKey)row.imgui_key, false))
            _tool.set_id(row.id);
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

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (io.KeyShift) _doc->redo();
        else             _doc->undo();
        _have_last = false;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        _doc->redo();
        _have_last = false;
    }
    if (io.KeyCtrl) return;

    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        std::vector<uint8_t> w((size_t)_doc->count(), 0);
        if (!io.KeyAlt)
            for (int64_t i = 0; i < _doc->count(); i++)
                if (_doc->alive()[i]) w[(size_t)i] = 255;
        _have_last = false;
        run_select(std::move(w),
                   io.KeyAlt ? msg::op_select_none : msg::op_select_all);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        std::vector<uint8_t> w = _doc->sel().weights();
        for (int64_t i = 0; i < _doc->count(); i++)
            w[(size_t)i] = _doc->alive()[i] ? (uint8_t)(255 - w[(size_t)i]) : 0;
        _have_last = false;
        run_select(std::move(w), msg::op_select_invert);
    }
    if (!_doc->sel().empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_X, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        _doc->run(make_hide_op(*_doc, false));
        _have_last = false;
    }
}


void EditSession::draw_status() {
    if (!_doc) return;
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

    // ---- tools ----
    ui::SeparatorText(msg::sec_tool);
    {
        constexpr int kPerRow = 3;
        const float w = (full - st.ItemSpacing.x * (kPerRow - 1)) / kPerRow;
        int col = 0;
        for (int i = 0; i < kNumTools; i++) {
            if (col) ImGui::SameLine();
            const ToolRow& row = tool_table()[i];
            if (toggle_button(tool_label(row.id), _tool.id() == row.id, w,
                              row.key))
                _tool.set_id(row.id);
            ui::help_on_hover(tool_hint(row.id));
            if (++col == kPerRow) col = 0;
        }
    }
    if (_tool.id() == ToolId::Brush) {
        float r = _tool.brush_radius();
        slider_item_width(msg::opt_brush_size, full);
        if (ui::SliderFloat(msg::opt_brush_size, &r, 2.0f, 300.0f, "%.0f"))
            _tool.set_brush_radius(r);
    }

    // ---- the set ----
    ui::SeparatorText(msg::sec_select);
    ui::Text(msg::stat_selected, {(long long)d.sel().count()});
    ImGui::SameLine();
    ui::TextDisabled(d.element_name());
    if (ui::Button(msg::act_all, ImVec2(third, 0))) {
        std::vector<uint8_t> sel((size_t)d.count(), 0);
        for (int64_t i = 0; i < d.count(); i++)
            if (d.alive()[i]) sel[(size_t)i] = 255;
        _have_last = false;
        run_select(std::move(sel), msg::op_select_all);
    }
    ImGui::SameLine();
    if (ui::Button(msg::act_none, ImVec2(third, 0))) {
        _have_last = false;
        run_select(std::vector<uint8_t>((size_t)d.count(), 0),
                   msg::op_select_none);
    }
    ImGui::SameLine();
    if (ui::Button(msg::act_invert, ImVec2(third, 0))) {
        std::vector<uint8_t> sel = d.sel().weights();
        for (int64_t i = 0; i < d.count(); i++)
            sel[(size_t)i] = d.alive()[i] ? (uint8_t)(255 - sel[(size_t)i]) : 0;
        _have_last = false;
        run_select(std::move(sel), msg::op_select_invert);
    }

    // What a new selection does to the one already there. The modifiers do
    // the same thing, which is what the tooltip says rather than a mode.
    {
        const Msg* labels[kNumCombine] = {&msg::combine_replace, &msg::combine_add,
                                          &msg::combine_subtract,
                                          &msg::combine_intersect};
        for (int i = 0; i < kNumCombine; i++) {
            if (i) ImGui::SameLine();
            if (ui::RadioButton(*labels[i], _combine == i)) _combine = i;
        }
        ui::help_on_hover(msg::combine_help);
    }

    if (ui::Checkbox(msg::opt_front_only, &_opt.front_only)) reapply_depth();
    ui::help_on_hover(msg::opt_front_only_help);
    if (d.kind() == EditDoc::Kind::Splats) {
        if (ui::Checkbox(msg::opt_by_extent, &_opt.by_extent)) reapply_depth();
        ui::help_on_hover(msg::opt_by_extent_help);
    }
    if (ui::Checkbox(msg::opt_depth_limit, &_opt.depth_limit)) reapply_depth();
    ui::help_on_hover(msg::opt_depth_limit_help);
    if (_opt.depth_limit) {
        slider_item_width(msg::opt_depth_near, full);
        if (ui::SliderFloat(msg::opt_depth_near, &_opt.near_frac, 0.0f, 1.0f,
                            "%.2f"))
            reapply_depth();
        slider_item_width(msg::opt_depth_far, full);
        if (ui::SliderFloat(msg::opt_depth_far, &_opt.far_frac, 0.0f, 1.0f,
                            "%.2f"))
            reapply_depth();
    }

    if (ui::Button(msg::act_grow, ImVec2(half, 0))) grow_shrink(true);
    ImGui::SameLine();
    if (ui::Button(msg::act_shrink, ImVec2(half, 0))) grow_shrink(false);
    if (ui::Button(msg::act_floaters, ImVec2(full, 0)))
        keep_largest_components(std::max(1, _keep_components));
    ui::help_on_hover(msg::act_floaters_help);

    if (ui::CollapsingHeader(msg::sec_advanced)) {
        slider_item_width(msg::act_reach, full);
        ui::SliderFloat(msg::act_reach, &_radius_mul, 0.25f, 12.0f, "%.2f");
        ui::help_on_hover(msg::act_reach_help);
        slider_item_width(msg::act_pieces_kept, full);
        ui::SliderInt(msg::act_pieces_kept, &_keep_components, 1, 32);
        slider_item_width(msg::opt_front_tol, full);
        if (ui::SliderFloat(msg::opt_front_tol, &_opt.front_tol, 0.0f, 0.5f,
                            "%.3f"))
            reapply_depth();
        ui::help_on_hover(msg::opt_front_tol_help);
        if (d.kind() == EditDoc::Kind::Splats) {
            slider_item_width(msg::opt_extent_scale, full);
            if (ui::SliderFloat(msg::opt_extent_scale, &_opt.extent_scale,
                                0.25f, 4.0f, "%.2f"))
                reapply_depth();
        }
    }

    // ---- what is done with it ----
    ui::SeparatorText(msg::sec_actions);
    ImGui::BeginDisabled(d.sel().empty());
    if (ui::Button(msg::act_delete, ImVec2(half, 0))) {
        d.run(make_hide_op(d, false));
        _have_last = false;
    }
    ui::help_on_hover_disabled(d.sel().empty() ? msg::stat_nothing_selected
                                               : msg::act_delete_help);
    ImGui::SameLine();
    if (ui::Button(msg::act_isolate, ImVec2(half, 0))) {
        d.run(make_hide_op(d, true));
        _have_last = false;
    }
    ui::help_on_hover_disabled(d.sel().empty() ? msg::stat_nothing_selected
                                               : msg::act_isolate_help);
    ImGui::EndDisabled();
    const int64_t hidden = d.count() - d.alive_count();
    ImGui::BeginDisabled(hidden == 0);
    if (ui::Button(msg::act_restore, ImVec2(full, 0))) {
        d.run(make_reveal_op(d));
        _have_last = false;
    }
    ImGui::EndDisabled();
    ui::Text(msg::stat_kept, {(long long)d.alive_count(), (long long)d.count()});
    if (hidden) ui::TextDisabled(msg::stat_hidden, {(long long)hidden});

    // ---- history ----
    ui::SeparatorText(msg::sec_history);
    ImGui::BeginDisabled(!d.can_undo());
    if (ui::Button(msg::hist_undo, ImVec2(half, 0))) {
        d.undo();
        _have_last = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!d.can_redo());
    if (ui::Button(msg::hist_redo, ImVec2(half, 0))) {
        d.redo();
        _have_last = false;
    }
    ImGui::EndDisabled();
    if (const Msg* m = d.undo_name()) ui::TextDisabled(*m);
    else ui::TextDisabled(msg::hist_empty);

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
    if (ui::Button(msg::save_over, ImVec2(half, 0))) save_to(_save_target, home);
    ImGui::EndDisabled();
    if (!home.empty()) ui::help_on_hover(msg::save_over_help, {home});
    ImGui::SameLine();
    ImGui::BeginDisabled(target.folder || !_pick_save);
    if (ui::Button(msg::save_copy, ImVec2(half, 0))) {
        std::string stem =
            std::filesystem::path(home.empty() ? d.source_path() : home)
                .stem()
                .string();
        if (stem.empty()) stem = "model";
        _pick_save(_save_target, target.ext, target.folder,
                   stem + "_edited" + target.ext);
    }
    ImGui::EndDisabled();
    if (d.kind() == EditDoc::Kind::Points && target.folder)
        ui::TextDisabledWrapped(msg::sparse_edit_help);
    if (d.dirty()) ui::TextDisabled(msg::save_unsaved);
    if (!_status.empty())
        ui::TextColoredWrappedRaw(_status_err ? ImVec4(1, 0.5f, 0.5f, 1)
                                              : ImVec4(0.6f, 0.9f, 0.6f, 1),
                                  _status);
}

}  // namespace gui
