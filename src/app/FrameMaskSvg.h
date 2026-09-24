#pragma once

// A frame stencil's shapes as an SVG file, in normalized image coordinates
// (viewBox "0 0 1 1", y down). Painted in order over a base rect, black removes
// and white keeps, so any SVG viewer shows the mask it rasterizes to. The
// format and how to add a shape kind: docs/notes/frame-stencil.md.

#include "app/FrameMask.h"

#include <string>
#include <vector>

namespace app {

// `title` becomes <title>, the name a saved stencil is listed under.
std::string write_mask_svg(const std::vector<MaskShape>& shapes, const std::string& title = "");

// Also reads hand-made SVG (rect, circle, ellipse, polygon, polyline, line,
// path; any viewBox), curves flattened, one shape per subpath. False, with
// `error`, on anything it cannot place -- a transform, say.
bool read_mask_svg(const std::string& text, std::vector<MaskShape>& out, std::string& title,
                   std::string& error);

bool load_mask_svg(const std::string& path, std::vector<MaskShape>& out, std::string& title,
                   std::string& error);
bool save_mask_svg(const std::string& path, const std::vector<MaskShape>& shapes,
                   const std::string& title, std::string& error);

// One subpath of an SVG path's `d`, curves flattened, in the path's own units.
struct SvgSubpath {
    std::vector<float> pts;   // x,y pairs
    bool closed = false;
};
bool parse_svg_path(const std::string& d, std::vector<SvgSubpath>& out, std::string& error);

}  // namespace app
