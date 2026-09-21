#pragma once

// Render mode: what a viewer pane embeds to make a photo or a video of what
// it shows. The camera move is edited on the pane itself -- keyframes drawn
// as cameras, moved with the editor's G / R / S grammar -- and on the
// timeline under it; the frame it will produce is previewed beside it.
// docs/notes/render-video.md.
//
// It reaches the pane through ViewportInteractor, as the editor does, and the
// models through the SourceViews its owner hands it every frame.

#include "app/gui/ViewportInput.h"
#include "app/gui/edit/TransformTool.h"
#include "app/gui/render/FrameRenderer.h"
#include "app/gui/render/FrameSink.h"
#include "app/gui/render/LensPresets.h"
#include "app/gui/render/RenderProject.h"
#include "app/gui/render/Trajectory.h"
#include "i18n/Message.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gui {

class ViewportPanel;

namespace render {

// What the owner knows about one model beside the SourceView itself.
struct SourceInfo {
    SourceView view;
    std::string name;                   // for lists: a file name, never translated
    std::string path;                   // what the project records
    // Splats and meshes: the dataset the model came from, when known, for
    // its cameras and lenses.
    const ParsedDataset* dataset = nullptr;
    // Which way is up in the model's file frame, when a dataset says.
    bool has_up = false;
    double up[3] = {0, 0, 1};
};

class RenderSession : public ViewportInteractor {
public:
    RenderSession();
    ~RenderSession() override;

    // Take over `panel`, the primary model's pane. With no project yet, one
    // starts from what the pane is looking at.
    void open(ViewportPanel* panel);
    // Stop drawing on the pane; the project and its history stay. A switch
    // to the editor autosaves without saying so.
    void close(bool switching = false);
    bool active() const { return _panel != nullptr; }

    // Supplied by the owner once a frame, before poll(). [0] is the primary.
    void set_sources(std::vector<SourceInfo> sources);
    // The primary model's file coordinates into the pane's shared frame:
    // base placement after the model's own normalization.
    void set_world_to_shared(const spirula::Sim3& w2s) { _w2s = w2s; }

    // Where file pickers come from: the owner has the dialog. `kind` is
    // what the answer is for (Pick).
    enum class Pick { SaveProject = 0, OpenProject, Output, AddModel };
    void set_pick(std::function<void(Pick, const std::string& start,
                                     const std::string& suggested)> f) {
        _pick = std::move(f);
    }
    void picked(Pick kind, const std::string& path);
    // Asked of the owner: go to the editor with this pane.
    void set_switch_to_edit(std::function<void()> f) { _to_edit = std::move(f); }
    void set_leave(std::function<void()> f) { _leave = std::move(f); }
    void set_ffmpeg(const std::string& exe) { _ffmpeg = exe; }

    const Output& output() const { return _project.output; }
    std::vector<std::string> drain_log();

    // Once a frame: sources, preview, the export in flight, the history.
    void poll();
    void draw_panel();
    // The strip under the panes: transport, ruler, keys, shots.
    void draw_timeline();
    float timeline_height() const;
    // The camera's own view, in the pane's space or on its own.
    enum class PreviewMode { Corner = 0, Beside, Through };
    PreviewMode preview_mode() const { return _preview_mode; }
    void draw_preview_pane();
    void draw_status();

    // The project on disk has changes it does not.
    bool dirty() const;
    // Rendering: the owner keeps the pane's model where it is meanwhile.
    bool exporting() const { return _job.state != Job::Idle; }
    // Playing back or exporting: the window keeps drawing.
    bool animating() const;

    bool owns_left_button() const override;
    bool blocks_fly_keys() const override;
    bool owns_right_button() const override { return _xform.active(); }
    bool on_viewport_input(const ViewportInput& in) override;
    void draw_viewport_overlay(const ViewportOverlay& v) override;

private:
    // ---- the project and its history ----
    void new_project();
    void commit_history();
    void undo();
    void redo();
    void project_changed();
    const Trajectory& trajectory();
    void save_to(const std::string& path);
    spirula::Sim3 primary_placement() const;
    void open_from(const std::string& path);

    // ---- keyframes ----
    int add_key_from_view(double time, bool select);
    void update_key_from_view(int index);
    void look_through(double time);
    void delete_selected();
    void select_only(int index);
    bool selected(int index) const;
    int single_selected() const;
    void space_evenly(double total);
    void make_orbit();
    void follow_capture();
    // The pane's navigation camera as a keyframe pose, world frame.
    bool view_pose(double pos[3], double rot[4], double target[3]) const;

    // ---- the viewport ----
    bool view_projection(ViewProjection& out, int W, int H) const;
    void to_shared(const double w[3], double s[3]) const;
    bool key_screen(int index, const ViewProjection& vp, float& x, float& y) const;
    int hit_key(float x, float y) const;
    // The operator's frame: pivot, axes, units. `kind` decides the pivot --
    // one aimed camera turns about what it looks at.
    bool xform_frame(XformFrame& f, XformKind kind);
    void begin_xform(XformKind kind, float mx, float my, bool drag, int axis = -1,
                     bool plane = false);
    void apply_xform(const spirula::Sim3& step_shared, bool scale_fov, double factor);
    void handle_keys(bool over_view);
    void draw_camera(ImDrawList* dl, const ViewProjection& vp, float ox,
                     float oy, const CameraState& c, unsigned col, float size,
                     bool axes) const;

    // ---- frames ----
    FrameSpec frame_spec(double t, int W, int H, bool photo);
    void preview_size(float box_w, float box_h, int& W, int& H) const;
    void request_preview();

    // ---- the export ----
    struct Job {
        enum State { Idle, Preparing, Rendering, Finishing } state = Idle;
        bool photo = false;
        int frame = 0, frames = 0;
        std::unique_ptr<FrameSink> sink;
        std::thread finisher;
        std::atomic<bool> finished{false};
        bool ok = true;
        std::string path, error;
        double started = 0.0;
        void reset() {
            state = Idle;
            photo = false;
            frame = frames = 0;
            sink.reset();
            finished = false;
            ok = true;
            path.clear();
            error.clear();
            started = 0.0;
        }
    };
    void start_export();
    void cancel_export();
    void poll_export();
    Encoder pick_encoder();
    void probe_encoder();

    // ---- panel pieces (RenderPanel.cpp) ----
    void draw_output_section(float full);
    void draw_keys_section(float full);
    void draw_lens_section(float full);
    void draw_motion_section(float full);
    void draw_effects_section(float full);
    void draw_project_section(float full);
    void note(const std::string& s);

    ViewportPanel* _panel = nullptr;
    RenderProject _project;
    bool _have_project = false;
    // Up came from a dataset or from the user, not from the frame's +Z.
    bool _up_known = false;
    void take_up_from_view();
    std::vector<SourceInfo> _sources;
    spirula::Sim3 _w2s;
    std::string _project_path;
    std::string _saved_json;             // what _project_path holds
    std::string _stable_json;            // the last committed state
    std::vector<std::string> _undo, _redo;
    uint64_t _revision = 1;
    std::unique_ptr<Trajectory> _traj;
    uint64_t _traj_rev = 0;

    std::vector<uint8_t> _sel;           // one flag per key
    double _time = 0.0;                  // the playhead, seconds
    bool _playing = false;
    double _play_from = 0.0, _play_clock = 0.0;
    bool _loop = true;

    // Preview.
    FrameRenderer _frames;
    PreviewMode _preview_mode = PreviewMode::Corner;
    float _preview_scale = 0.5f;         // of the output size, capped
    std::string _preview_key;
    bool _preview_wanted = true;
    int _preview_w = 0, _preview_h = 0;
    float _through_drag[2] = {0, 0};

    // Viewport interaction.
    TransformTool _xform;
    XformKind _handle_mode = XformKind::Move;
    int _hot = -1;                       // key under the pointer
    int _handle_hot = -1;                // TransformTool handle under it
    std::vector<Keyframe> _xform_from;   // the keys as the operator found them
    bool _pick_target = false;
    bool _pick_waiting = false;
    int _fly_block_key = 0;
    float _mouse[2] = {0, 0};
    bool _mouse_in = false;

    // Timeline.
    int _drag_key = -1;
    double _drag_key_from = 0.0;
    bool _scrubbing = false;
    float _timeline_zoom = 1.0f;

    Job _job;
    std::atomic<int> _encoder_probe{0};  // 0 unknown, 1 probing, 2 built-in works, 3 not
    std::atomic<int> _encoder_codecs{0}; // bit 0 H.264, bit 1 H.265
    bool builtin_encodes(bool h265) const;
    std::thread _probe;
    std::string _ffmpeg = "ffmpeg";

    std::vector<DatasetLens> _dataset_lenses;
    std::string _dataset_lenses_key;

    std::function<void(Pick, const std::string&, const std::string&)> _pick;
    std::function<void()> _to_edit, _leave;
    std::vector<std::string> _log;
    std::string _status;
    bool _status_err = false;
};

}  // namespace render
}  // namespace gui
