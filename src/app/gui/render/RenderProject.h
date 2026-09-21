#pragma once

// A camera move and everything a render of it needs, kept as one JSON file:
// the keyframes, the lens, the output, the models shown and how the picture
// passes from one to the next. docs/notes/render-video.md.
//
// Poses are in the PRIMARY model's saved coordinates with OpenGL camera axes
// (x right, y up, looking down -z), so a project re-renders against a model
// retrained from the same dataset. Unknown JSON keys are ignored and absent
// ones keep their defaults, which is what lets the format grow.

#include "core/Similarity.h"

#include <string>
#include <vector>

namespace gui::render {

// The camera models every renderer here knows; the values are
// CameraModelType's (core/CameraModel.h), which this header cannot include
// next to the engine's copy of the same enum.
enum class Projection { Perspective = 0, Fisheye, Equisolid, Equirect };
constexpr int kNumProjections = 4;
// The width of a distortion row, core/CameraModel.h's kCameraDistortionParams.
constexpr int kLensCoeffs = 8;

struct Lens {
    Projection projection = Projection::Perspective;
    // fx / image width: resolution-free, and the pixels stay square. An
    // equirectangular lens ignores it -- its image is the whole sphere.
    double focal = 0.8660254;
    // CameraDistortionType, and its coefficients in that tier's own order.
    int tier = 0;
    float dist[kLensCoeffs] = {};

    bool operator==(const Lens& o) const;
    bool operator!=(const Lens& o) const { return !(*this == o); }
};

// Across the image width, degrees, ignoring the distortion: what a field of
// view slider shows. The setter keeps the projection and the distortion.
double lens_fov(const Lens& l);
void lens_set_fov(Lens& l, double degrees);
// The 35 mm-equivalent focal length of a perspective lens: a full-frame
// sensor is 36 mm across.
inline double lens_mm(const Lens& l) { return 36.0 * l.focal; }
// Pixels for an image `w` x `h`: fx, fy, cx, cy.
void lens_intrinsics(const Lens& l, int w, int h, float out[4]);

struct Keyframe {
    double time = 0.0;                  // seconds
    double pos[3] = {0, 0, 0};
    double rot[4] = {1, 0, 0, 0};       // camera-to-world, (w, x, y, z)
    // Aimed at `target`: `rot` is then derived from the position, the target,
    // the project's up and `roll`, and kept in step with them.
    bool aim = false;
    double target[3] = {0, 0, 0};
    double roll = 0.0;                  // degrees about the view axis
    // False: the lens of the key before carries on. The first key always
    // has its own.
    bool own_lens = false;
    Lens lens;
    // The camera comes to rest here instead of passing through.
    bool hold = false;
};

// How the picture passes from one shot to the next. Wipe, iris and sweep
// are shapes; grow is the model assembling itself (docs/notes/render-video.md).
enum class Transition {
    Cut = 0, Crossfade, DipBlack, DipWhite, WipeLeft, WipeRight, WipeUp,
    WipeDown, Iris, Sweep, Grow
};
constexpr int kNumTransitions = 11;

// From `start` on, `source` is what is shown, entering by `transition` over
// `duration` seconds. Source -1 is the background alone.
struct Shot {
    double start = 0.0;
    int source = 0;
    Transition transition = Transition::Cut;
    double duration = 1.0;
};

enum class PointStyle { Square = 0, Circle, Gaussian, Sphere };
constexpr int kNumPointStyles = 4;

// How one model is drawn. Each field means something for one kind only.
struct SourceStyle {
    PointStyle point_style = PointStyle::Circle;
    float point_px = 3.0f;              // screen styles, pixels at 1080 lines
    float sphere_radius = 0.004f;       // Sphere, fraction of the scene's size
    bool cameras = false;               // a reconstruction's own cameras
    bool shade = true, flat = false, colour = true;   // meshes
    int sh_degree = -1;                 // splats; < 0 = all the file has
    // Splats: a `--primitive` name, or empty for what the viewport shows.
    std::string primitive;
};

struct Source {
    std::string path;                   // what was opened, as the viewer took it
    SourceStyle style;
};

enum class OutputKind { Photo = 0, Video, Frames };
// Photos and frames; PngAlpha keeps what is not the model transparent.
enum class ImageFormat { Png = 0, PngAlpha, Jpeg };
constexpr int kNumImageFormats = 3;
// Videos: an MP4 in one of three codecs, or an animated GIF.
enum class Codec { H264 = 0, H265, Av1, Gif };
constexpr int kNumCodecs = 4;

struct Output {
    OutputKind kind = OutputKind::Video;
    int width = 1920, height = 1080;
    double fps = 30.0;
    ImageFormat format = ImageFormat::Png;
    int jpeg_quality = 95;
    Codec codec = Codec::H264;
    int quality = 1;                    // 0 best, 1 standard, 2 smallest
    std::string path;                   // where the last one went
};

// How the keys are joined. Spline is C2 through every key; Catmull-Rom is
// the older local curve, which `tension` tightens.
enum class Curve { Spline = 0, CatmullRom, Linear };
constexpr int kNumCurves = 3;

struct Motion {
    Curve curve = Curve::Spline;
    bool ease = true;                   // start from rest and come to rest
    bool constant_speed = false;        // the same speed all the way along
    // After the last key the camera goes back to the first, and the video
    // ends where it began.
    bool loop = false;
    double tension = 0.0;               // Catmull-Rom: 0 loose .. 1 a stop at every key
};

enum class FadeColour { None = 0, Black, White };
struct Fade {
    FadeColour colour = FadeColour::None;
    double seconds = 1.0;
};

struct RenderProject {
    std::vector<Keyframe> keys;         // kept sorted by time
    Output output;
    Motion motion;
    Fade fade_in, fade_out;
    float background[3] = {0, 0, 0};
    // Which way is up, for an aimed key: +Z of the frame the model was
    // opened in, as the viewport shows it.
    double up[3] = {0, 0, 1};
    std::vector<Source> sources;        // [0] is the primary model
    std::vector<Shot> shots;            // empty: the primary model throughout
    // A loop's end, back at the first key; 0 = one average key spacing on.
    double end = 0.0;
    // The editor's placement of the primary model, file coordinates, that the
    // poses were laid out against: opened against another, the whole move is
    // carried across the difference.
    spirula::Sim3 placement;

    double duration() const;
    bool looped() const { return motion.loop && keys.size() >= 2; }
    // The lens in force at key `i`: its own, or the last one before it.
    const Lens& lens_at(int i) const;
    // Sorted by time. A key that becomes the first keeps the lens it was
    // seen through rather than taking a default one.
    void sort_keys();
};

// Rotation (w, x, y, z) of a camera at `pos` looking at `target` with `up` on
// top, turned `roll` degrees about the view axis.
void aim_rotation(const double pos[3], const double target[3],
                  const double up[3], double roll, double out[4]);
// The roll that aim_rotation would need to reproduce `rot` at this aim.
double roll_of(const double rot[4], const double pos[3], const double target[3],
               const double up[3]);
// Refresh an aimed key's rotation after its position, target or roll moved.
void update_aim(Keyframe& k, const double up[3]);

// Rigidly move the whole trajectory: positions and targets through `s`,
// rotations by its rotation. A model turned in the editor takes its camera
// move with it this way.
void transform_project(RenderProject& p, const spirula::Sim3& s);

std::string project_to_json(const RenderProject& p);
// Throws std::runtime_error on a file that is not a project.
RenderProject project_from_json(const std::string& text);
RenderProject load_project(const std::string& path);
void save_project(const RenderProject& p, const std::string& path);

// The folder a project for a model is suggested into: `renders/` beside the
// model when there is one, else in the dataset it was trained from, else a new
// one beside the model.
std::string default_project_dir(const std::string& model_path);

// A model saved with the editor's placement `move` baked in takes its camera
// moves along: moved copies of `from_model`'s projects go into `renders/`
// beside `to_model`, the originals stay. Returns how many; `dir` gets where.
int copy_moved_projects(const std::string& from_model, const std::string& to_model,
                        const spirula::Sim3& move, std::string& dir);

}  // namespace gui::render
