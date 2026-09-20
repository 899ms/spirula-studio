#pragma once

// What a screen embeds to make a viewport editable: the document, the active
// tool, the panel they are driven from, and the glue that turns a drag into a
// selection. One per editable pane; the owner drives poll() once a frame.
//
// It reaches the viewport through ViewportInteractor alone, so the panel
// knows nothing about editing and this knows nothing about how the panel
// renders.

#include "app/gui/ViewportInput.h"
#include "app/gui/edit/EditDoc.h"
#include "app/gui/edit/EditTool.h"
#include "app/gui/edit/ElementGrid.h"
#include "app/gui/edit/SelectShape.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gui {

class ViewportPanel;

class EditSession : public ViewportInteractor {
public:
    ~EditSession() override;

    // Takes the document over and installs itself on `panel`.
    void open(std::unique_ptr<EditDoc> doc, ViewportPanel* panel);
    void close();
    bool active() const { return _doc != nullptr; }
    EditDoc* doc() { return _doc.get(); }

    // Where a file picker comes from: the owner has the dialog. `folder` asks
    // for a directory rather than a file; `suggested` is the name to open a
    // save under.
    void set_pick_save(std::function<void(int target, const std::string& ext,
                                          bool folder,
                                          const std::string& suggested)> f) {
        _pick_save = std::move(f);
    }
    // The owner's answer, once the user has chosen.
    void save_to(int target, const std::string& path);

    // Anything the session wants said in the log panel.
    std::vector<std::string> drain_log();

    // Once a frame, before the viewport draws.
    void poll();
    // The editing panel: tools, options, selection, actions, history.
    void draw_panel();
    // One line under the viewport: the active tool and its keys.
    void draw_status();

    bool owns_left_button() const override { return _tool.owns_pointer(); }
    bool on_viewport_input(const ViewportInput& in) override;
    void draw_viewport_overlay(const ViewportOverlay& v) override;

private:
    void apply_stroke(const ShapeStroke& s, const ViewportInput& in);
    void run_select(std::vector<uint8_t> w, const spirula::i18n::Msg& name);
    void reapply_depth();
    Combine combine_now(bool shift, bool ctrl) const;
    float reach();
    void ensure_grid();
    void grow_shrink(bool grow);
    void select_component_under(float px, float py);
    void keep_largest_components(int keep);
    void handle_keys();
    void note(const std::string& s);
    bool view(ViewProjection& out) const;

    std::unique_ptr<EditDoc> _doc;
    ViewportPanel* _panel = nullptr;
    EditTool _tool;
    ElementGrid _grid;
    SelectOptions _opt;
    int _combine = 0;                 // Combine, when no modifier is held
    int _save_target = 0;
    float _radius_mul = 2.0f;
    float _spacing = 0.0f;            // measured once per document
    int _keep_components = 1;

    // The last stencil run, kept so the depth sliders can re-trim it without
    // the user drawing the shape again.
    bool _have_last = false;
    ShapeStroke _last_shape;
    ViewProjection _last_view;
    Combine _last_combine = Combine::Replace;
    std::vector<uint8_t> _before_last;
    SelectResult _last_result;

    OcclusionBuffer _occ;
    bool _occ_dirty = true;
    float _occ_pose[12] = {};

    std::function<void(int, const std::string&, bool, const std::string&)>
        _pick_save;
    std::vector<std::string> _log;
    std::string _status;              // formatted, already translated
    bool _status_err = false;
    int64_t _occ_alive = -1;          // the live count the buffer was built at
};

}  // namespace gui
