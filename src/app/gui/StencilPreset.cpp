// StencilPreset.cpp -- see StencilPreset.h.

#include "app/gui/StencilPreset.h"

#include "app/FrameMaskSvg.h"
#include "app/gui/PresetFile.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace gui {

std::string stencil_preset_dir() {
    // The sibling of the dataset presets that name these.
    const fs::path d = fs::path(preset_dir(PresetKind::Dataset)).parent_path() / "stencil";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d.string();
}

static std::string file_for(const std::string& name) {
    return fs::path(preset_file_name(name)).replace_extension(".svg").string();
}

std::vector<StencilPreset> list_stencil_presets() {
    std::vector<StencilPreset> out;
    std::error_code ec;
    for (fs::directory_iterator it(stencil_preset_dir(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".svg") continue;
        std::vector<app::MaskShape> shapes;
        std::string title, err;
        if (!app::load_mask_svg(it->path().string(), shapes, title, err)) continue;
        out.push_back({title.empty() ? it->path().stem().string() : title, it->path().string()});
    }
    std::sort(out.begin(), out.end(),
              [](const StencilPreset& a, const StencilPreset& b) { return a.name < b.name; });
    return out;
}

bool load_stencil_preset(const std::string& name_or_path, std::vector<app::MaskShape>& out,
                         std::string& error) {
    std::string path;
    for (const StencilPreset& p : list_stencil_presets())
        if (p.name == name_or_path) path = p.path;
    std::error_code ec;
    if (path.empty() && fs::is_regular_file(name_or_path, ec)) path = name_or_path;
    if (path.empty()) {
        const fs::path by_file = fs::path(stencil_preset_dir()) / file_for(name_or_path);
        if (fs::is_regular_file(by_file, ec)) path = by_file.string();
    }
    if (path.empty()) {
        error = name_or_path;
        return false;
    }
    std::string title;
    return app::load_mask_svg(path, out, title, error);
}

bool save_stencil_preset(const std::string& name, const std::vector<app::MaskShape>& shapes,
                         std::string& path, std::string& error) {
    path.clear();
    for (const StencilPreset& p : list_stencil_presets())
        if (p.name == name) path = p.path;
    if (path.empty()) path = (fs::path(stencil_preset_dir()) / file_for(name)).string();
    return app::save_mask_svg(path, shapes, name, error);
}

std::vector<std::string> save_dataset_stencils(
    const std::string& workspace,
    const std::vector<std::pair<std::string, std::vector<app::MaskShape>>>& inputs,
    std::string& error) {
    std::vector<std::string> written;
    const fs::path dir = fs::path(workspace) / kDatasetStencilDir;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".svg") fs::remove(it->path(), ec);
    const std::string dataset = fs::path(workspace).filename().string();
    for (const auto& [input, shapes] : inputs) {
        if (shapes.empty()) continue;
        fs::create_directories(dir, ec);
        // The input's own name; a folder called images/ says little, so its parent's too.
        fs::path in = fs::path(input);
        if (!in.has_filename()) in = in.parent_path();
        std::string stem = in.stem().string();
        if (stem == "images" && in.has_parent_path()) stem = in.parent_path().filename().string() + "-" + stem;
        const std::string base = fs::path(preset_file_name(stem)).stem().string();
        fs::path file = dir / (base + ".svg");
        for (int k = 2; fs::exists(file, ec); k++)
            file = dir / (base + "-" + std::to_string(k) + ".svg");
        if (!app::save_mask_svg(file.string(), shapes, dataset + " - " + in.filename().string(),
                                error))
            return written;
        written.push_back(file.string());
    }
    return written;
}

std::vector<StencilPreset> list_dataset_stencils(const std::string& workspace) {
    std::vector<StencilPreset> out;
    if (workspace.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(workspace) / kDatasetStencilDir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".svg") continue;
        std::vector<app::MaskShape> shapes;
        std::string title, err;
        if (!app::load_mask_svg(it->path().string(), shapes, title, err)) continue;
        out.push_back({title.empty() ? it->path().stem().string() : title, it->path().string()});
    }
    std::sort(out.begin(), out.end(),
              [](const StencilPreset& a, const StencilPreset& b) { return a.name < b.name; });
    return out;
}

}  // namespace gui
