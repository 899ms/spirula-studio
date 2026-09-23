#pragma once

// Draws a PathTool over a canvas: the committed polyline, the live segment
// to the cursor, the anchors, and the first anchor lit when a click would
// close on it. The ImGui half of the pen tool; PathTool.cpp has none.

struct ImDrawList;
struct ImVec2;

namespace gui {
namespace mask {

class PathTool;

void draw_path_overlay(ImDrawList* dl, const ImVec2& origin, const PathTool& tool);

}  // namespace mask
}  // namespace gui
