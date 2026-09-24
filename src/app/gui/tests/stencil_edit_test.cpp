// stencil_edit_test -- app/gui/StencilEdit.h and StencilPreset.h with no GUI:
// what each tool's stroke becomes, hit tests and moves, undo, and saving a
// set of drawn areas and finding it again by name.

#include "app/gui/StencilEdit.h"
#include "app/gui/StencilPreset.h"
#include "core/SourcePath.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Kind = app::MaskShape::Kind;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

bool near_(float a, float b) { return std::fabs(a - b) < 1e-5f; }

gui::ShapeStroke stroke(gui::ShapeKind k, std::vector<float> pts, float r = 0.0f) {
    gui::ShapeStroke s;
    s.kind = k;
    s.pts = std::move(pts);
    s.brush_radius = r;
    return s;
}

void test_from_stroke() {
    app::MaskShape m;
    // A 400 x 200 canvas.
    check(gui::stencil_shape_from_stroke(stroke(gui::ShapeKind::Box, {300, 150, 100, 50}),
                                         400, 200, true, m) &&
              m.kind == Kind::Rect && m.remove && near_(m.cx, 0.25f) && near_(m.cy, 0.25f) &&
              near_(m.rx, 0.75f) && near_(m.ry, 0.75f),
          "box: corners sorted and normalized");
    check(gui::stencil_shape_from_stroke(stroke(gui::ShapeKind::Ellipse, {100, 50, 300, 150}),
                                         400, 200, false, m) &&
              m.kind == Kind::Ellipse && !m.remove && near_(m.cx, 0.5f) && near_(m.rx, 0.25f) &&
              near_(m.ry, 0.25f),
          "ellipse: centre and radii per axis; the keep flag passes through");
    check(!gui::stencil_shape_from_stroke(stroke(gui::ShapeKind::Box, {10, 10, 11, 40}), 400,
                                          200, true, m),
          "a box one pixel wide is a click, not a shape");
    check(gui::stencil_shape_from_stroke(
              stroke(gui::ShapeKind::Polygon, {0, 0, 400, 0, 200, 200}), 400, 200, true, m) &&
              m.kind == Kind::Path && m.pts.size() == 6 && near_(m.pts[2], 1.0f) &&
              near_(m.pts[5], 1.0f),
          "polygon: a path in normalized corners");
    check(!gui::stencil_shape_from_stroke(stroke(gui::ShapeKind::Lasso, {0, 0, 10, 10}), 400,
                                          200, true, m),
          "a lasso of two points is not a shape");

    // A brush 10 canvas px round on a 2:1 canvas is 10 px on each axis.
    std::vector<float> drag;
    for (int i = 0; i <= 100; i++) drag.insert(drag.end(), {100.0f + i, 100.0f});
    check(gui::stencil_shape_from_stroke(stroke(gui::ShapeKind::Brush, drag, 10.0f), 400, 200,
                                         true, m) &&
              m.kind == Kind::Stroke && near_(m.rx, 10.0f / 400) && near_(m.ry, 10.0f / 200),
          "brush: round in canvas pixels, so rx and ry differ on a 2:1 canvas");
    check(m.pts.size() >= 4 && m.pts.size() < drag.size() / 2 &&
              near_(m.pts[m.pts.size() - 2], 200.0f / 400),
          "brush: every-frame samples thinned, the last point kept (" +
              std::to_string(m.pts.size() / 2) + " points)");
}

void test_hit_and_move() {
    app::MaskShape s;
    s.kind = Kind::Stroke;
    s.rx = 0.05f;
    s.ry = 0.1f;
    s.pts = {0.2f, 0.5f, 0.8f, 0.5f};
    check(gui::stencil_contains(s, 0.5f, 0.59f) && !gui::stencil_contains(s, 0.5f, 0.61f) &&
              gui::stencil_contains(s, 0.84f, 0.5f) && !gui::stencil_contains(s, 0.86f, 0.5f),
          "stroke hit: within ry above the line, rx past the end");
    gui::stencil_move(s, 0.1f, -0.1f);
    check(near_(s.pts[0], 0.3f) && near_(s.pts[3], 0.4f), "stroke moves by its points");
    float u[3], v[3];
    check(gui::stencil_handles(s, u, v) == 0, "a stroke has no resize handles");

    app::MaskShape r;
    r.kind = Kind::Rect;
    r.cx = 0.1f; r.cy = 0.1f; r.rx = 0.3f; r.ry = 0.2f;
    check(gui::stencil_handles(r, u, v) == 3 && near_(u[0], 0.2f) && near_(v[0], 0.15f),
          "rect: the first handle is its centre");
    gui::stencil_move_handle(r, 0, 0.5f, 0.5f);
    check(near_(r.cx, 0.4f) && near_(r.ry, 0.55f), "rect: the centre handle moves it whole");
}

void test_history() {
    gui::StencilHistory h;
    std::vector<app::MaskShape> shapes;
    app::MaskShape a;
    h.push(shapes);
    shapes.push_back(a);
    h.push(shapes);
    shapes.push_back(a);
    check(h.undo(shapes) && shapes.size() == 1, "undo takes back the last add");
    check(h.undo(shapes) && shapes.empty() && !h.can_undo(), "and the one before");
    check(h.redo(shapes) && shapes.size() == 1 && h.can_redo(), "redo puts it back");
    h.push(shapes);
    shapes.clear();
    check(!h.can_redo(), "a new change drops what redo held");
}

void test_presets() {
    const fs::path root = fs::temp_directory_path() / "ss_stencil_edit_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", root.string().c_str(), 1);
#endif
    std::vector<app::MaskShape> shapes(1);
    shapes[0].kind = Kind::Rect;
    shapes[0].remove = true;
    shapes[0].cx = 0.0f; shapes[0].cy = 0.9f; shapes[0].rx = 1.0f; shapes[0].ry = 1.0f;
    std::string path, err;
    check(gui::save_stencil_preset("Selfie stick", shapes, path, err), "save: " + err);
    check(fs::path(path).extension() == ".svg" &&
              fs::path(path).parent_path() == fs::path(gui::stencil_preset_dir()),
          "saved as an .svg in the stencil folder: " + path);
    const auto list = gui::list_stencil_presets();
    check(list.size() == 1 && list[0].name == "Selfie stick", "listed by its title");
    std::vector<app::MaskShape> back;
    check(gui::load_stencil_preset("Selfie stick", back, err) && back.size() == 1 &&
              back[0].kind == Kind::Rect && near_(back[0].cy, 0.9f),
          "loaded back by name");
    check(gui::load_stencil_preset(path, back, err), "and by path");
    shapes.push_back(shapes[0]);
    check(gui::save_stencil_preset("Selfie stick", shapes, path, err) &&
              gui::list_stencil_presets().size() == 1 &&
              gui::load_stencil_preset("Selfie stick", back, err) && back.size() == 2,
          "saving the same name replaces it");
    check(!gui::load_stencil_preset("nothing by this name", back, err),
          "an unknown name is refused");

    // A run keeps its inputs' shapes beside the dataset, one file each.
    const fs::path ws = root / "capture_dataset";
    fs::create_directories(ws / gui::kDatasetStencilDir, ec);
    { std::ofstream stale((ws / gui::kDatasetStencilDir / "old.svg").string()); stale << "<svg/>"; }
    err.clear();
    const auto written = gui::save_dataset_stencils(
        ws.string(),
        {{(ws / "images").string(), shapes}, {"/videos/front.mp4", {}}, {"/videos/back.mp4", shapes}},
        err);
    check(written.size() == 2 && err.empty(), "dataset stencils: one file per input that drew");
    check(!fs::exists(ws / gui::kDatasetStencilDir / "old.svg"),
          "dataset stencils: the folder holds only this run's");
    const auto kept = gui::list_dataset_stencils(ws.string());
    check(kept.size() == 2 && gui::load_stencil_preset(kept[0].path, back, err) && back.size() == 2,
          "dataset stencils: listed and loadable by path");
    check(fs::path(written[0]).filename() == "capture-dataset-images.svg" &&
              fs::path(written[1]).filename() == "back.svg",
          "dataset stencils: named after the input: " + fs::path(written[0]).filename().string());
    check(gui::list_dataset_stencils("").empty(), "dataset stencils: no workspace, nothing listed");
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    test_from_stroke();
    test_hit_and_move();
    test_history();
    test_presets();
    std::printf("%s: %d failure(s)\n", SS_FILE, g_failures);
    return g_failures;
}
