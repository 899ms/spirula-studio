#pragma once

// The seam an editing tool plugs the viewport into.
//
// ViewportPanel draws an image and navigates a camera; a tool needs the
// pointer over that image and a place to draw on top of it, and nothing else
// about either. Kept in its own header so the panel does not have to know
// what a tool is.

struct ImDrawList;

namespace gui {

// One frame of pointer state, in the pixels the image was drawn at (0,0 at
// its top-left corner).
struct ViewportInput {
    bool hovered = false;
    float x = 0, y = 0;
    int W = 1, H = 1;
    bool down = false, clicked = false, released = false;
    bool right_clicked = false, double_clicked = false;
    bool shift = false, ctrl = false, alt = false;
};

// Where the image landed on screen this frame.
struct ViewportOverlay {
    ImDrawList* dl = nullptr;
    float x = 0, y = 0, w = 0, h = 0;
};

// An interaction that owns the viewport's LEFT button while it is installed;
// the other two stay with navigation, so a tool is never a dead end
// (docs/notes/gui-editing-plan.md, "Mouse conventions").
struct ViewportInteractor {
    virtual ~ViewportInteractor() = default;
    // Whether the active tool claims the left button at all. False means the
    // panel navigates exactly as it does with no tool installed.
    virtual bool owns_left_button() const = 0;
    // True when the tool took this frame's left button.
    virtual bool on_viewport_input(const ViewportInput& in) = 0;
    virtual void draw_viewport_overlay(const ViewportOverlay& v) = 0;
};

}  // namespace gui
