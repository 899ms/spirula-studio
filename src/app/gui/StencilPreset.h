#pragma once

// Saved "drawn areas": a frame stencil's shapes as an SVG file
// (app/FrameMaskSvg.h) in <config>/presets/stencil, listed by its <title>. A
// dataset preset names one (DatasetSettings::frame_shapes), which is how a
// batch run gets them. The fitted fisheye border is never in one.

#include "app/FrameMask.h"

#include <string>
#include <utility>
#include <vector>

namespace gui {

struct StencilPreset {
    std::string name;
    std::string path;
};

std::string stencil_preset_dir();
std::vector<StencilPreset> list_stencil_presets();

// By the name the list shows, or by a path to any .svg.
bool load_stencil_preset(const std::string& name_or_path, std::vector<app::MaskShape>& out,
                         std::string& error);
// Replaces a saved one of the same name. `path` is the file written.
bool save_stencil_preset(const std::string& name, const std::vector<app::MaskShape>& shapes,
                         std::string& path, std::string& error);

// A run keeps each input's drawn shapes beside the dataset, so they outlive
// the session that drew them even if nobody saved them.
inline constexpr const char* kDatasetStencilDir = "frame_stencil";
// (input path, shapes) per input; one SVG each, named after the input. The
// folder is rewritten, so it holds exactly what this run drew. Returns the
// files written; an input with no shapes writes none.
std::vector<std::string> save_dataset_stencils(
    const std::string& workspace,
    const std::vector<std::pair<std::string, std::vector<app::MaskShape>>>& inputs,
    std::string& error);
std::vector<StencilPreset> list_dataset_stencils(const std::string& workspace);

}  // namespace gui
