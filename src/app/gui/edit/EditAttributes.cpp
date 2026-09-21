// EditAttributes.cpp -- the editing session's "select by what it is" half:
// the brushable histogram over app/gui/edit/Attributes.h and the colour
// sampler. The session is in EditSession.h.

#include "app/gui/edit/EditSession.h"

#include "app/gui/Layout.h"
#include "app/gui/Ui.h"
#include "i18n/catalog/Edit.h"
#include "i18n/catalog/EditAttributes.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace msg = spirula::i18n::msg::attr;
namespace emsg = spirula::i18n::msg::edit;
using spirula::i18n::Msg;

namespace gui {

namespace {

// Live preview while a range or a tolerance is being dragged: often enough to
// steer by, seldom enough that a colour upload per frame is not the cost.
constexpr double kPreviewEvery = 0.07;
constexpr int kAdjustRange = 1, kAdjustColour = 2;

ImU32 bar_colour(AttrTint tint, float frac) {
    switch (tint) {
        case AttrTint::Red:   return IM_COL32(200, 90, 100, 255);
        case AttrTint::Green: return IM_COL32(110, 180, 80, 255);
        case AttrTint::Blue:  return IM_COL32(80, 140, 220, 255);
        case AttrTint::Gray: {
            const int v = 60 + (int)(frac * 180.0f);
            return IM_COL32(v, v, v, 255);
        }
        case AttrTint::Hue: {
            float r, g, b;
            ImGui::ColorConvertHSVtoRGB(frac, 0.75f, 0.85f, r, g, b);
            return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
        }
        default: return IM_COL32(130, 140, 155, 255);
    }
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

}  // namespace


// ---------------------------------------------------------------------------
// A selection that is adjusted rather than stacked
// ---------------------------------------------------------------------------

void EditSession::begin_adjustable(int kind) {
    if (!_doc || _adjust_live) return;
    // Dragging the same control again straight after: the step it made is
    // taken back, so the new one replaces it instead of meeting its result.
    if (_adjust_kind == kind && _adjust_head >= 0 &&
        _adjust_head == _doc->history_head() && _doc->can_undo())
        _doc->undo();
    _adjust_kind = kind;
    _adjust_before = _doc->sel().weights();
    _adjust_live = true;
    _preview_at = 0.0;
}

void EditSession::preview_adjustable(const std::vector<uint8_t>& w) {
    if (!_doc || !_adjust_live) return;
    const double now = ImGui::GetTime();
    if (now - _preview_at < kPreviewEvery) return;
    _preview_at = now;
    Selection tmp;
    tmp.assign(_adjust_before);
    const ImGuiIO& io = ImGui::GetIO();
    tmp.combine(w.data(), combine_now(io.KeyShift, io.KeyCtrl), _doc->alive());
    _doc->set_selection(tmp.weights());
    _doc->mark_display_dirty();
}

void EditSession::commit_adjustable(const std::vector<uint8_t>& w,
                                    const std::string& label) {
    if (!_doc || !_adjust_live) return;
    _adjust_live = false;
    Selection tmp;
    tmp.assign(_adjust_before);
    const ImGuiIO& io = ImGui::GetIO();
    tmp.combine(w.data(), combine_now(io.KeyShift, io.KeyCtrl), _doc->alive());
    // The preview wrote the selection without a step; the step has to start
    // from what was there before it.
    _doc->set_selection(_adjust_before);
    _doc->run(make_select_op(*_doc, tmp.weights(), label, {}));
    _adjust_head = _doc->history_head();
}


// ---------------------------------------------------------------------------
// By attribute
// ---------------------------------------------------------------------------

void EditSession::refresh_attribute() {
    if (!_doc) return;
    if (_attrs_layer != _doc->layer() || _attrs.empty()) {
        _attrs = attributes_of(*_doc);
        _attrs_layer = _doc->layer();
        _attr = 0;
        _hist_attr = -1;
        _range_set = false;
    }
    if (_attrs.empty()) return;
    _attr = std::clamp(_attr, 0, (int)_attrs.size() - 1);
    const bool new_attr = _hist_attr != (int)_attrs[(size_t)_attr];
    if (!new_attr && _hist_rev == _doc->revision()) return;
    const Attr a = _attrs[(size_t)_attr];
    // The nearest-camera distance is the one attribute that costs more than
    // a pass over the elements, and the only thing that moves it is a camera.
    const bool keep_values = !new_attr && a == Attr::CameraDistance &&
                             _attr_values.size() == (size_t)_doc->count();
    if (!keep_values && !attribute_values(*_doc, a, _attr_values)) {
        _attr_values.clear();
        return;
    }
    const AttrInfo& info = attr_info(a);
    _hist.build(_attr_values, _doc->alive(), _doc->sel().data(), info.log,
                info.lo, info.hi);
    _hist_attr = (int)a;
    _hist_rev = _doc->revision();
    if (new_attr) _range_set = false;
}

void EditSession::draw_attribute_section(float full) {
    refresh_attribute();
    if (_attrs.empty() || _attr_values.empty()) {
        ui::TextDisabledWrapped(msg::none_here);
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    const AttrInfo& info = attr_info(_attrs[(size_t)_attr]);

    ImGui::SetNextItemWidth(full);
    if (ui::BeginComboRaw("##attr", info.name->get())) {
        for (int i = 0; i < (int)_attrs.size(); i++)
            if (ui::Selectable(*attr_info(_attrs[(size_t)i]).name, i == _attr))
                _attr = i;
        ImGui::EndCombo();
    }
    ui::help_on_hover(*info.help);

    // ---- the plot ----
    const float h = px(96.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ui::InvisibleButtonRaw("##histplot", ImVec2(full, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActivated();
    const bool held = ImGui::IsItemActive();
    const bool let_go = ImGui::IsItemDeactivated();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + full, p0.y + h);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, 255), px(3.0f));

    const float peak = _hist_log_counts ? std::log1p((float)_hist.peak)
                                        : (float)_hist.peak;
    if (peak > 0.0f) {
        const int cols = std::max(1, (int)full);
        for (int x = 0; x < cols; x++) {
            // A column can cover several bins or a bin several columns; the
            // tallest bin under it is what a thin spike needs to stay visible.
            const int b0 = x * AttrHistogram::kBins / cols;
            const int b1 = std::max(b0 + 1, (x + 1) * AttrHistogram::kBins / cols);
            uint32_t all = 0, sel = 0;
            for (int b = b0; b < b1 && b < AttrHistogram::kBins; b++) {
                all = std::max(all, _hist.all[(size_t)b]);
                sel = std::max(sel, _hist.selected[(size_t)b]);
            }
            if (!all) continue;
            auto height = [&](uint32_t c) {
                const float v = _hist_log_counts ? std::log1p((float)c) : (float)c;
                return std::max(1.0f, v / peak * (h - px(4.0f)));
            };
            const float fx = (float)x / (float)cols;
            dl->AddRectFilled(ImVec2(p0.x + x, p1.y - height(all)),
                              ImVec2(p0.x + x + 1, p1.y), bar_colour(info.tint, fx));
            if (sel)
                dl->AddRectFilled(ImVec2(p0.x + x, p1.y - height(sel)),
                                  ImVec2(p0.x + x + 1, p1.y), IM_COL32(255, 120, 20, 255));
        }
    }

    // ---- the range ----
    const float mx = (ImGui::GetIO().MousePos.x - p0.x) / std::max(full, 1.0f);
    const double mf = std::clamp((double)mx, 0.0, 1.0);
    const float grab = px(6.0f) / std::max(full, 1.0f);
    if (pressed) {
        begin_adjustable(kAdjustRange);
        if (_range_set && std::fabs(mf - _range[0]) < grab) _range_drag = 1;
        else if (_range_set && std::fabs(mf - _range[1]) < grab) _range_drag = 2;
        else {
            _range_drag = 3;
            _range_anchor = mf;
            _range[0] = _range[1] = mf;
            _range_set = true;
        }
    }
    auto tool_answer = [&](std::vector<uint8_t>& w) {
        select_by_range(_attr_values, _hist, _range[0], _range[1], _range_outside,
                        _doc->alive(), w);
    };
    auto label = [&] {
        return spirula::i18n::format(
            msg::op_select_by,
            {info.name->get(), number(_hist.value_at(std::min(_range[0], _range[1]))),
             number(_hist.value_at(std::max(_range[0], _range[1])))});
    };
    if (held && _range_drag) {
        if (_range_drag == 1) _range[0] = mf;
        else if (_range_drag == 2) _range[1] = mf;
        else {
            _range[0] = std::min(_range_anchor, mf);
            _range[1] = std::max(_range_anchor, mf);
        }
        if (_range[0] > _range[1]) {
            std::swap(_range[0], _range[1]);
            if (_range_drag != 3) _range_drag = 3 - _range_drag;
        }
        std::vector<uint8_t> w;
        tool_answer(w);
        preview_adjustable(w);
    }
    if (let_go && _range_drag) {
        _range_drag = 0;
        std::vector<uint8_t> w;
        tool_answer(w);
        commit_adjustable(w, label());
    }
    if (_range_set) {
        const float x0 = p0.x + (float)_range[0] * full, x1 = p0.x + (float)_range[1] * full;
        const ImU32 wash = IM_COL32(255, 255, 255, 34);
        if (_range_outside) {
            dl->AddRectFilled(p0, ImVec2(x0, p1.y), wash);
            dl->AddRectFilled(ImVec2(x1, p0.y), p1, wash);
        } else {
            dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), wash);
        }
        for (float x : {x0, x1}) {
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 220), px(1.5f));
            dl->AddRectFilled(ImVec2(x - px(3.0f), p0.y + h * 0.4f),
                              ImVec2(x + px(3.0f), p0.y + h * 0.6f),
                              IM_COL32(255, 255, 255, 230), px(2.0f));
        }
    }
    if (hovered && !held) {
        dl->AddLine(ImVec2(p0.x + (float)mf * full, p0.y),
                    ImVec2(p0.x + (float)mf * full, p1.y), IM_COL32(255, 255, 255, 70));
        ui::SetTooltipRaw(number(_hist.value_at(mf)));
    }

    // The ends of the axis, and what the range is in the attribute's units.
    ui::TextDisabledRaw(number(_hist.value_at(0.0)));
    const std::string top = number(_hist.value_at(1.0));
    ImGui::SameLine(full - ImGui::CalcTextSize(top.c_str()).x);
    ui::TextDisabledRaw(top);

    if (_range_set) {
        // Typed ends, for a threshold somebody already knows.
        const float fw = (full - st.ItemSpacing.x) * 0.5f;
        float lo = (float)_hist.value_at(_range[0]), hi = (float)_hist.value_at(_range[1]);
        bool changed = false;
        ImGui::SetNextItemWidth(fw);
        ui::InputFloatRaw("##rangelo", &lo, "%.4g");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fw);
        ui::InputFloatRaw("##rangehi", &hi, "%.4g");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (changed) {
            _range[0] = std::clamp(_hist.frac_of(std::min(lo, hi)), 0.0, 1.0);
            _range[1] = std::clamp(_hist.frac_of(std::max(lo, hi)), 0.0, 1.0);
            begin_adjustable(kAdjustRange);
            std::vector<uint8_t> w;
            tool_answer(w);
            commit_adjustable(w, label());
        }
    } else {
        ui::TextDisabledWrapped(msg::range_hint);
    }

    if (ui::Checkbox(msg::range_outside, &_range_outside) && _range_set) {
        begin_adjustable(kAdjustRange);
        std::vector<uint8_t> w;
        tool_answer(w);
        commit_adjustable(w, label());
    }
    ui::help_on_hover(msg::range_outside_help);
    ImGui::SameLine();
    ui::Checkbox(msg::log_counts, &_hist_log_counts);
    ui::help_on_hover(msg::log_counts_help);
}


// ---------------------------------------------------------------------------
// By colour
// ---------------------------------------------------------------------------

void EditSession::run_colour(bool commit) {
    if (!_doc || _samples.empty()) return;
    const uint64_t key = ((uint64_t)_doc->layer() << 48) ^ (uint64_t)_doc->count();
    if (_colours_key != key || _colours.size() != (size_t)_doc->count() * 3) {
        if (!_doc->colours(_colours)) {
            _colours.clear();
            return;
        }
        _colours_key = key;
    }
    std::vector<uint8_t> w;
    select_by_colour(_colours, _samples.data(), (int)_samples.size() / 3,
                     _colour_tol, _colour_light, _doc->alive(), w);
    begin_adjustable(kAdjustColour);
    if (commit) commit_adjustable(w, msg::op_select_colour.get());
    else preview_adjustable(w);
}

void EditSession::pick_colour(float px_, float py_, bool append) {
    ViewProjection vp;
    if (!_doc || !view(vp)) return;
    const int64_t hit = pick_element(*_doc, vp, px_, py_, 16.0f);
    if (hit < 0) return;
    std::vector<float> rgb;
    if (!_doc->colours(rgb) || (int64_t)rgb.size() < (hit + 1) * 3) return;
    // Shift adds a sample: a sky is a gradient, and one click is one blue.
    if (!append) {
        _samples.clear();
        // A fresh colour is a new selection, not a correction of the last.
        _adjust_head = -1;
    }
    if (_samples.size() >= 8 * 3) _samples.erase(_samples.begin(), _samples.begin() + 3);
    _samples.insert(_samples.end(), rgb.begin() + hit * 3, rgb.begin() + hit * 3 + 3);
    _adjust_live = false;
    run_colour(/*commit=*/true);
}

void EditSession::draw_colour_section(float full) {
    if (!_doc->colours_available()) {
        ui::TextDisabledWrapped(msg::no_colour_here);
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    ui::TextDisabledWrapped(msg::colour_hint);

    bool edited = false;
    for (int i = 0; i < (int)_samples.size() / 3; i++) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        edited |= ui::ColorEdit3Raw("##sample", &_samples[(size_t)i * 3],
                                    ImGuiColorEditFlags_NoInputs |
                                        ImGuiColorEditFlags_NoLabel |
                                        ImGuiColorEditFlags_Float |
                                        ImGuiColorEditFlags_HDR);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            _samples.erase(_samples.begin() + i * 3, _samples.begin() + i * 3 + 3);
            edited = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (!_samples.empty()) ImGui::SameLine();
    if (ui::Button(msg::colour_add)) {
        _samples.insert(_samples.end(), {0.5f, 0.5f, 0.5f});
        edited = true;
    }
    ui::help_on_hover(msg::colour_add_help);

    float label_w = std::max(ImGui::CalcTextSize(msg::colour_tolerance.get()).x,
                             ImGui::CalcTextSize(msg::colour_lightness.get()).x);
    const float sw = std::max(full - label_w - st.ItemInnerSpacing.x, full * 0.3f);
    bool live = false, done = false;
    ImGui::SetNextItemWidth(sw);
    live |= ui::SliderFloat(msg::colour_tolerance, &_colour_tol, 0.005f, 0.5f, "%.3f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ui::help_on_hover(msg::colour_tolerance_help);
    ImGui::SetNextItemWidth(sw);
    live |= ui::SliderFloat(msg::colour_lightness, &_colour_light, 0.0f, 1.0f, "%.2f");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ui::help_on_hover(msg::colour_lightness_help);

    if (_samples.empty()) return;
    if (done || edited) run_colour(true);
    else if (live) run_colour(false);
}

}  // namespace gui
