#pragma once

// Editing a frame stencil's shapes by hand, with no ImGui: what a finished
// tool stroke becomes, how a selected shape is hit, moved and resized, and
// undo. SegmentPanel is the UI over it. A new shape kind (a pen tool's
// curves, say) goes here and in app/FrameMaskSvg.h: docs/notes/frame-stencil.md.

#include "app/FrameMask.h"
#include "app/gui/edit/SelectShape.h"

#include <vector>

namespace gui {

// `s` was drawn on a canvas of cw x ch pixels that shows the whole frame, in
// the mask editor's shape grammar. False for a stroke too small to be a shape.
bool stencil_shape_from_stroke(const ShapeStroke& s, float cw, float ch, bool remove,
                               app::MaskShape& out);

// Draggable points in normalized coordinates: the first moves the shape, the
// rest resize it. Paths and strokes have only the first.
int stencil_handles(const app::MaskShape& s, float u[3], float v[3]);
bool stencil_contains(const app::MaskShape& s, float u, float v);
void stencil_move(app::MaskShape& s, float du, float dv);
void stencil_move_handle(app::MaskShape& s, int handle, float u, float v);

// Snapshots of the whole list: a stencil is a handful of shapes.
class StencilHistory {
public:
    // Call with the shapes as they were BEFORE the change.
    void push(const std::vector<app::MaskShape>& before);
    bool undo(std::vector<app::MaskShape>& shapes);
    bool redo(std::vector<app::MaskShape>& shapes);
    bool can_undo() const { return !_undo.empty(); }
    bool can_redo() const { return !_redo.empty(); }
    void clear();

private:
    std::vector<std::vector<app::MaskShape>> _undo, _redo;
};

}  // namespace gui
