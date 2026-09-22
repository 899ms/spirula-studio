#pragma once

// One frame of a RenderProject, for the preview and for the file: each model
// drawn by its own renderer (the engine for splats, GL for points and
// meshes) and the layers composited on the GPU with the shot's transition,
// the fades and the background. docs/notes/render-video.md.
//
// GUI thread only -- the GL work happens in poll(); the splat renders run on
// RenderWorker threads under the engine lock the viewer panes share.

#include "app/gui/PreviewRenderer.h"
#include "app/gui/render/Trajectory.h"
#include "app/webviewer/RenderWorker.h"
#include "core/Similarity.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gui::render {

// A model as the render sees it, filled in by the owner of the panes.
struct SourceView {
    enum Kind { Splats = 0, Points, Mesh };
    Kind kind = Splats;
    std::string key;                    // what is loaded; a change rebuilds
    // The model's normalized frame (what its renderer is given) into the
    // project's world, and its file's coordinates into that frame.
    spirula::Sim3 norm_to_world;
    spirula::Sim3 file_to_norm;
    // Splats: how the viewer renders them, and the file an effect reads back.
    ViewerRenderConfig cfg;
    ViewerHooks hooks;
    std::string primitive;              // what the viewport renders it as
    std::string file;
    int sh_max = 0;
    // Splats being edited: which survive, or empty when all do.
    std::shared_ptr<const std::vector<uint8_t>> alive;
    // Points and meshes, valid while the owner's viewer holds them.
    const ParsedDataset* ds = nullptr;
    const PostSplitCameras* post = nullptr;
    const meshing::MeshData* mesh = nullptr;
    float mesh_t2n[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

// A 3D transition acting on one layer's points, splats or vertices
// (TransitionFx.h), with the scene it moves in the world frame.
struct LayerFx {
    int kind = 0;                       // a Transition from Dust on; 0 none
    bool incoming = false;
    float t = 0.0f;
    float p[2] = {0.0f, 0.0f};
    double centre[3] = {0, 0, 0}, up[3] = {0, 0, 1};
    double radius = 1.0, h0 = -1.0, h1 = 1.0;
};

// One model in one frame, and what an effect is doing to it. A model whose
// look changes between keyframes is drawn both ways and mixed on screen:
// `style[1]` by `style_mix`, when `variants` is 2.
struct LayerSpec {
    int source = -1;                    // -1: nothing
    SourceStyle style[2];
    int variants = 1;
    float style_mix = 0.0f;
    float grow = 1.0f;                  // splat and point size, 1 = as made
    float fade_in = 1.0f;               // splat opacity while growing in
    // Keep only one side of a level along the project's up axis, world
    // units, with `glow` before the cut lit up.
    int clip = 0;                       // 0 none, 1 keep below, 2 keep above
    double level = 0.0;
    float glow = 0.0f;
    float glow_col[3] = {1.0f, 0.86f, 0.6f};
    LayerFx fx;
};

// Where a model is, robust to what floats far from it: the per-axis median,
// the 90th-percentile distance from it, and the 2nd / 98th-percentile heights
// along up relative to it; world frame.
struct SceneStats {
    double centre[3] = {0, 0, 0};
    double radius = 1.0, h0 = -1.0, h1 = 1.0;
};

struct FrameSpec {
    CameraState cam;
    int width = 0, height = 0;
    double up[3] = {0, 0, 1};
    LayerSpec a, b;                     // a under b
    // 0: mix(a, b) by `mix` through `mask` (0 all, 1 wipe, 2 iris);
    // 1: b over a (a 3D transition, where each has moved itself);
    // 2: a zooms out of the picture as b zooms in, by `zoom`.
    int mode = 0;
    int mask = 0;
    float mix = 1.0f;
    float wipe_dir[2] = {1, 0};         // image space, y down
    float soft = 0.01f;                 // of a wipe's or an iris's edge
    float zoom = 0.0f;
    // Two tints over everything: a dip, then a fade; rgb + amount.
    float tint[2][4] = {};
    float background[3] = {0, 0, 0};
    bool transparent = false;
};

class FrameRenderer {
public:
    FrameRenderer();
    ~FrameRenderer();

    // The models on offer. Unchanged keys keep their renderers.
    void set_sources(const std::vector<SourceView>& sources);
    int source_count() const { return (int)_slots.size(); }

    // Start a frame; one still in flight is abandoned.
    void request(const FrameSpec& f);
    // Forget the frame in flight, for a cancelled export.
    void abandon() { _stage = Stage::Idle; _passes.clear(); }
    // Collect the splat renders, draw the rest and composite. True once the
    // requested frame is finished; `wait` seconds may be spent blocking on
    // the engine, which an export wants and the preview does not.
    bool poll(double wait = 0.0);
    bool busy() const { return _stage == Stage::Rendering; }
    uint64_t frames_done() const { return _done; }

    // The last finished frame: a GL texture (rows bottom-up) and its pixels,
    // rows top-down, straight alpha when `alpha`, RGB otherwise.
    unsigned texture() const { return _out_tex; }
    int width() const { return _out_w; }
    int height() const { return _out_h; }
    void read(std::vector<uint8_t>& out, bool alpha);

    // Effects that rewrite splats need them host-side: read on demand, in
    // the background. True once `source` has them (or needs none).
    bool effects_ready(int source);
    // SceneStats of `source` along `up`; false until its elements are read.
    bool scene_stats(int source, const double up[3], SceneStats& out);

    std::string take_error();
    void destroy_gl();

private:
    struct SplatHost;
    struct Slot;
    enum class Stage { Idle, Rendering };

    // One render of one model: a layer, drawn in one of its looks. A splat
    // model's worker keeps a single request slot, so its passes go one at a
    // time -- two at once and the first would never come back.
    struct Pass {
        int layer = 0, variant = 0;
        uint64_t id = 0;                // worker request, 0 = not sent yet
        double sent = 0.0;
        bool ready = false;
        bool flip = false;              // rows top-down (a worker's) or GL's
        unsigned tex = 0;
    };
    void submit(Pass& p);
    unsigned draw_gl_layer(Slot& s, const LayerSpec& l, const SourceStyle& style);
    bool ensure_compositor();
    // `dst` becomes mix(a, b, w), rows GL's way up; `b` may be 0.
    void blend(unsigned a, bool flip_a, unsigned b, bool flip_b, float w, unsigned dst);
    unsigned target(int index);
    void composite();

    std::vector<std::unique_ptr<Slot>> _slots;
    FrameSpec _spec;
    Stage _stage = Stage::Idle;
    std::vector<Pass> _passes;
    unsigned _layer_tex[2] = {0, 0};
    bool _layer_flip[2] = {false, false};
    uint64_t _done = 0;
    std::string _error;

    unsigned _prog = 0, _vao = 0, _fbo = 0, _out_tex = 0;
    int _out_w = 0, _out_h = 0;
    int _u[16] = {};
    // The blend pass and the textures passes and layers land in.
    unsigned _blend_prog = 0, _blend_fbo = 0;
    int _bu[6] = {};
    unsigned _tex_pool[6] = {};
    int _pool_w = 0, _pool_h = 0;
};

}  // namespace gui::render
