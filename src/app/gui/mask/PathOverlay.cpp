// PathOverlay.cpp -- see PathOverlay.h.

#include "app/gui/mask/PathOverlay.h"

#include "app/gui/mask/PathTool.h"

#include "imgui.h"

#include <vector>

namespace gui {
namespace mask {

void draw_path_overlay(ImDrawList* dl, const ImVec2& o, const PathTool& tool) {
    if (!dl || !tool.in_progress()) return;
    const ImU32 line = IM_COL32(255, 190, 60, 230);
    const ImU32 soft = IM_COL32(255, 190, 60, 140);
    std::vector<float> anchors, committed, live;
    tool.overlay(anchors, committed, live);
    auto path = [&](const std::vector<float>& p, ImU32 col, float thick) {
        if (p.size() < 4) return;
        for (size_t i = 0; i + 1 < p.size(); i += 2)
            dl->PathLineTo(ImVec2(o.x + p[i], o.y + p[i + 1]));
        dl->PathStroke(col, 0, thick);
    };
    path(committed, line, 2.0f);
    path(live, soft, 1.5f);
    for (size_t i = 2; i + 1 < anchors.size(); i += 2)
        dl->AddCircleFilled(ImVec2(o.x + anchors[i], o.y + anchors[i + 1]), 3.0f, line);
    // The first anchor is the one that closes the loop: bigger, and lit when
    // the cursor is near enough to hit it, as EditTool's polygon does.
    const bool can_close = tool.anchor_count() >= kPathMinAnchors && tool.near_first();
    const ImVec2 first(o.x + anchors[0], o.y + anchors[1]);
    dl->AddCircleFilled(first, can_close ? 8.0f : 5.0f,
                        can_close ? IM_COL32(120, 255, 140, 255) : line);
    if (can_close) dl->AddCircle(first, 12.0f, IM_COL32(120, 255, 140, 200), 0, 2.0f);
}

}  // namespace mask
}  // namespace gui
