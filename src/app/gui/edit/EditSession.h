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

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gui {

class ViewportPanel;

// What produced a selection. Kept on the history step it made, so a setting
// changed at that point in the history re-runs THAT selection -- however many
// undos the user walked back to reach it.
struct SelectRecipe {
    enum Kind { Stencil, Grow, Shrink, Piece, Floaters };
    Kind kind = Stencil;
    int layer = 0;
    std::vector<uint8_t> before;
    Combine combine = Combine::Replace;
    ShapeStroke shape;          // Stencil
    ViewProjection view;        // Stencil
    int64_t seed = -1;          // Piece
};

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
    // Save over what the document came from, which is what the panel's Save
    // button and the quit dialog both mean.
    void save_in_place();
    // Ask for a place to save, the way the panel's "Save a copy" does.
    void ask_save_copy();
    bool can_save_in_place() const;
    bool can_save_copy() const;

    // A setting the tools read, changed through the history so that Ctrl+Z
    // puts it back and the selection it produced with it. Public because the
    // panel's widgets are what move them.
    void set_option(bool* slot, bool value, const spirula::i18n::Msg& name);
    void set_number(float* slot, float value, const spirula::i18n::Msg& name);
    void set_number(int* slot, int value, const spirula::i18n::Msg& name);

    // Anything the session wants said in the log panel.
    std::vector<std::string> drain_log();

    // Once a frame, before the viewport draws.
    void poll();
    // The editing panel: tools, options, selection, actions, history.
    void draw_panel();
    // One line under the viewport: the active tool and its keys.
    void draw_status();

    bool owns_left_button() const override { return _tool.owns_pointer(); }
    // The letter that switched tools is usually still down on the frame the
    // switch takes effect, so Q would both select Navigate and fly the camera
    // once. The block lifts when that key comes up.
    bool blocks_fly_keys() const override {
        return _tool.owns_pointer() || _fly_block_key != 0;
    }
    bool on_viewport_input(const ViewportInput& in) override;
    void draw_viewport_overlay(const ViewportOverlay& v) override;
    // A long job is in flight; editing waits for it.
    bool busy() const { return _comp_busy.load() || _save_busy.load(); }
    // Give up on it. The worker checks between cells, so this is not instant.
    void cancel_work();
    // How far along, for the bar the panel draws.
    int work_done() const { return _save_done.load(); }
    int work_total() const { return _save_total.load(); }

private:
    void apply_stroke(const ShapeStroke& s, const ViewportInput& in);
    // Compute the selection a recipe describes and either record it as a step
    // or write it straight in -- a setting change is its own step, so the
    // selection it re-derives must not be a second one.
    void run_recipe(const std::shared_ptr<const SelectRecipe>& r, bool push);
    void start_recipe(std::shared_ptr<SelectRecipe> r);
    void run_select(std::vector<uint8_t> w, std::string label);
    Combine combine_now(bool shift, bool ctrl) const;
    void set_layer(int i);
    float reach();
    void ensure_grid();
    // The labels for the current layer, live set and reach, computed on a
    // worker the first time they are asked for; the same answer then serves
    // every click after it.
    bool components_ready();
    void compute_components();
    void grow_shrink(bool grow);
    void select_component_under(float px, float py, Combine how);
    void keep_largest_components();
    void select_all(bool on);
    void invert_selection();
    void handle_keys();
    void note(const std::string& s);
    bool view(ViewProjection& out) const;
    // Wrap a setting change as a history step that re-derives the selection
    // the step it stands on produced.
    void run_setting(std::function<void(bool)> write, std::string label);

    std::unique_ptr<EditDoc> _doc;
    ViewportPanel* _panel = nullptr;
    EditTool _tool;
    ElementGrid _grid;
    SelectOptions _opt;
    int _combine = 0;                 // Combine, when no modifier is held
    int _save_target = 0;
    float _radius_mul = 2.0f;
    float _spacing = 0.0f;            // measured once per layer
    int _keep_components = 1;

    SelectResult _last_result;

    // A depth buffer over the live elements, built for one camera.
    OcclusionBuffer _occ;
    bool _occ_dirty = true;
    int64_t _occ_alive = -1;
    float _occ_pose[12] = {};

    // Connected components, cached against what they were computed from.
    struct Components {
        int layer = -1;
        int64_t alive = -1;
        float radius = -1.0f;
        std::vector<int32_t> label;
        std::vector<int64_t> sizes;
    };
    Components _comp;
    std::thread _comp_worker;
    std::atomic<bool> _comp_busy{false};
    std::atomic<bool> _cancel{false};
    // The recipe waiting on that worker, and whether it is a step of its own
    // or a re-derivation inside one.
    std::shared_ptr<SelectRecipe> _pending;
    bool _pending_push = true;
    int _fly_block_key = 0;

    // Saving, which for a linked mesh is several large files.
    std::thread _save_worker;
    std::atomic<bool> _save_busy{false};
    std::atomic<int> _save_done{0}, _save_total{0};
    std::string _save_error;

    std::function<void(int, const std::string&, bool, const std::string&)>
        _pick_save;
    std::vector<std::string> _log;
    std::string _status;              // formatted, already translated
    bool _status_err = false;
    // Set when the Save button is pressed; the panel puts the question up and
    // clears it when it is answered.
    bool _ask_overwrite = false;
};

}  // namespace gui
