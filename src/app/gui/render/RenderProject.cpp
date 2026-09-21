// RenderProject.cpp -- see RenderProject.h.

#include "app/gui/render/RenderProject.h"

#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gui::render {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr const char* kFormat = "spirula-render";
constexpr int kVersion = 1;

const char* const kProjectionNames[kNumProjections] = {
    "perspective", "fisheye", "equisolid", "equirectangular"};
const char* const kTransitionNames[kNumTransitions] = {
    "cut", "crossfade", "dip_black", "dip_white", "wipe_left", "wipe_right",
    "wipe_up", "wipe_down", "iris", "sweep", "grow"};
const char* const kPointStyleNames[kNumPointStyles] = {
    "square", "circle", "gaussian", "sphere"};
const char* const kOutputNames[3] = {"photo", "video", "frames"};
const char* const kFadeNames[3] = {"none", "black", "white"};

template <int N>
int name_index(const char* const (&names)[N], const std::string& s, int def) {
    for (int i = 0; i < N; i++)
        if (s == names[i]) return i;
    return def;
}

double dot3(const double a[3], const double b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
bool normalize3(double v[3]) {
    const double n = std::sqrt(dot3(v, v));
    if (!(n > 1e-300)) return false;
    for (int k = 0; k < 3; k++) v[k] /= n;
    return true;
}

// Row-major 3x3 with the camera axes as COLUMNS (x right, y up, z back).
void quat_from_columns(const double X[3], const double Y[3], const double Z[3],
                       double q[4]) {
    spirula::Sim3 s;
    for (int r = 0; r < 3; r++) {
        s.R[r*3+0] = X[r];
        s.R[r*3+1] = Y[r];
        s.R[r*3+2] = Z[r];
    }
    s.quat(q);
}

void quat_to_matrix(const double q[4], double R[9]) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0] = 1 - 2*(y*y + z*z); R[1] = 2*(x*y - w*z);     R[2] = 2*(x*z + w*y);
    R[3] = 2*(x*y + w*z);     R[4] = 1 - 2*(x*x + z*z); R[5] = 2*(y*z - w*x);
    R[6] = 2*(x*z - w*y);     R[7] = 2*(y*z + w*x);     R[8] = 1 - 2*(x*x + y*y);
}

// ---- JSON helpers ----

void write_vec(JsonWriter& w, const char* key, const double* v, int n) {
    w.key(key).array();
    for (int i = 0; i < n; i++) w.raw(json_number_exact(v[i]));
    w.end();
}
void write_vecf(JsonWriter& w, const char* key, const float* v, int n) {
    w.key(key).array();
    for (int i = 0; i < n; i++) w.raw(json_number_exact((double)v[i]));
    w.end();
}
bool read_vec(const JsonValue& o, const char* key, double* out, int n) {
    const JsonValue* v = o.find(key);
    if (!v || !v->is_array() || (int)v->arr.size() != n) return false;
    for (int i = 0; i < n; i++) out[i] = v->arr[(size_t)i].as_double(out[i]);
    return true;
}
bool read_vecf(const JsonValue& o, const char* key, float* out, int n) {
    double tmp[16];
    for (int i = 0; i < n; i++) tmp[i] = out[i];
    if (!read_vec(o, key, tmp, n)) return false;
    for (int i = 0; i < n; i++) out[i] = (float)tmp[i];
    return true;
}
std::string get_str(const JsonValue& o, const char* key) {
    const JsonValue* v = o.find(key);
    return v ? v->as_string() : std::string();
}
bool get_bool(const JsonValue& o, const char* key, bool def) {
    const JsonValue* v = o.find(key);
    return v ? v->as_bool(def) : def;
}
double get_num(const JsonValue& o, const char* key, double def) {
    const double v = o.get_double(key, def);
    return std::isfinite(v) ? v : def;
}

void write_lens(JsonWriter& w, const Lens& l) {
    w.object();
    w.field("projection", kProjectionNames[(int)l.projection]);
    w.key("focal").raw(json_number_exact(l.focal));
    w.field("distortion", l.tier);
    write_vecf(w, "coefficients", l.dist, kLensCoeffs);
    w.end();
}
Lens read_lens(const JsonValue& o) {
    Lens l;
    l.projection = (Projection)name_index(kProjectionNames, get_str(o, "projection"), 0);
    l.focal = std::max(get_num(o, "focal", l.focal), 1e-4);
    l.tier = std::clamp((int)get_num(o, "distortion", 0), 0, 2);
    read_vecf(o, "coefficients", l.dist, kLensCoeffs);
    return l;
}

}  // namespace


// ===========================================================================
// Lens
// ===========================================================================

bool Lens::operator==(const Lens& o) const {
    if (projection != o.projection || focal != o.focal || tier != o.tier)
        return false;
    for (int i = 0; i < kLensCoeffs; i++)
        if (dist[i] != o.dist[i]) return false;
    return true;
}

double lens_fov(const Lens& l) {
    const double f = std::max(l.focal, 1e-6);
    switch (l.projection) {
        case Projection::Equirect:  return 360.0;
        case Projection::Fisheye:   return 2.0 * (0.5 / f) * 180.0 / kPi;
        case Projection::Equisolid: {
            const double s = std::min(0.25 / f, 1.0);
            return 4.0 * std::asin(s) * 180.0 / kPi;
        }
        default: return 2.0 * std::atan(0.5 / f) * 180.0 / kPi;
    }
}

void lens_set_fov(Lens& l, double degrees) {
    const double half = std::max(degrees, 0.1) * 0.5 * kPi / 180.0;
    switch (l.projection) {
        case Projection::Equirect:  break;
        case Projection::Fisheye:   l.focal = 0.5 / half; break;
        case Projection::Equisolid: l.focal = 0.5 / (2.0 * std::sin(std::min(half, kPi) * 0.5)); break;
        default: l.focal = 0.5 / std::tan(std::min(half, 0.4999 * kPi)); break;
    }
}

void lens_intrinsics(const Lens& l, int w, int h, float out[4]) {
    if (l.projection == Projection::Equirect) {
        out[0] = (float)(w / (2.0 * kPi));
        out[1] = (float)(h / kPi);
    } else {
        out[0] = out[1] = (float)(l.focal * w);
    }
    out[2] = 0.5f * (float)w;
    out[3] = 0.5f * (float)h;
}


// ===========================================================================
// Project
// ===========================================================================

double RenderProject::duration() const {
    double t = keys.empty() ? 0.0 : keys.back().time;
    return end > 0.0 ? std::max(end, t) : t;
}

const Lens& RenderProject::lens_at(int i) const {
    static const Lens kDefault;
    if (keys.empty()) return kDefault;
    i = std::clamp(i, 0, (int)keys.size() - 1);
    for (int k = i; k > 0; k--)
        if (keys[(size_t)k].own_lens) return keys[(size_t)k].lens;
    return keys[0].lens;
}

void RenderProject::sort_keys() {
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    if (!keys.empty()) keys[0].own_lens = true;
}

void aim_rotation(const double pos[3], const double target[3],
                  const double up[3], double roll, double out[4]) {
    double f[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    if (!normalize3(f)) { out[0] = 1; out[1] = out[2] = out[3] = 0; return; }
    double u[3] = {up[0], up[1], up[2]};
    if (!normalize3(u)) { u[0] = 0; u[1] = 0; u[2] = 1; }
    double x[3];
    cross3(f, u, x);
    // Straight up or down has no "up" left in the picture; any sideways axis
    // will do, and +Y of the frame is the one a plan view puts on top.
    if (!normalize3(x)) {
        const double alt[3] = {0, 1, 0};
        cross3(f, alt, x);
        if (!normalize3(x)) { x[0] = 1; x[1] = 0; x[2] = 0; }
    }
    double y[3];
    cross3(x, f, y);
    const double a = roll * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    double xr[3], yr[3];
    for (int k = 0; k < 3; k++) {
        xr[k] = c * x[k] + s * y[k];
        yr[k] = -s * x[k] + c * y[k];
    }
    const double z[3] = {-f[0], -f[1], -f[2]};
    quat_from_columns(xr, yr, z, out);
}

double roll_of(const double rot[4], const double pos[3], const double target[3],
               const double up[3]) {
    double q0[4];
    aim_rotation(pos, target, up, 0.0, q0);
    double R0[9], R[9];
    quat_to_matrix(q0, R0);
    quat_to_matrix(rot, R);
    // The camera's own x against the unrolled x and y, both in the image plane.
    const double x[3] = {R[0], R[3], R[6]};
    const double x0[3] = {R0[0], R0[3], R0[6]}, y0[3] = {R0[1], R0[4], R0[7]};
    return std::atan2(dot3(x, y0), dot3(x, x0)) * 180.0 / kPi;
}

void update_aim(Keyframe& k, const double up[3]) {
    if (k.aim) aim_rotation(k.pos, k.target, up, k.roll, k.rot);
}

void transform_project(RenderProject& p, const spirula::Sim3& s) {
    spirula::Sim3 rot;
    for (int i = 0; i < 9; i++) rot.R[i] = s.R[i];
    double rq[4];
    rot.quat(rq);
    for (Keyframe& k : p.keys) {
        double q[3];
        s.apply(k.pos, q);
        for (int i = 0; i < 3; i++) k.pos[i] = q[i];
        s.apply(k.target, q);
        for (int i = 0; i < 3; i++) k.target[i] = q[i];
        // rq * k.rot, both (w, x, y, z).
        const double a0 = rq[0], a1 = rq[1], a2 = rq[2], a3 = rq[3];
        const double b0 = k.rot[0], b1 = k.rot[1], b2 = k.rot[2], b3 = k.rot[3];
        k.rot[0] = a0*b0 - a1*b1 - a2*b2 - a3*b3;
        k.rot[1] = a0*b1 + a1*b0 + a2*b3 - a3*b2;
        k.rot[2] = a0*b2 - a1*b3 + a2*b0 + a3*b1;
        k.rot[3] = a0*b3 + a1*b2 - a2*b1 + a3*b0;
    }
    double u[3];
    s.rotate(p.up, u);
    for (int i = 0; i < 3; i++) p.up[i] = u[i];
}


// ===========================================================================
// JSON
// ===========================================================================

std::string project_to_json(const RenderProject& p) {
    JsonWriter w;
    w.object();
    w.field("format", kFormat);
    w.field("version", kVersion);
    write_vec(w, "up", p.up, 3);
    write_vecf(w, "background", p.background, 3);
    if (p.end > 0.0) w.key("end").raw(json_number_exact(p.end));
    if (!p.placement.is_identity(1e-12)) {
        double a[12];
        p.placement.to_3x4(a);
        write_vec(w, "placement", a, 12);
    }

    w.key("output").object();
    w.field("kind", kOutputNames[(int)p.output.kind]);
    w.field("width", p.output.width);
    w.field("height", p.output.height);
    w.key("fps").raw(json_number_exact(p.output.fps));
    w.field("image_format", p.output.format == ImageFormat::Jpeg ? "jpeg" : "png");
    w.field("jpeg_quality", p.output.jpeg_quality);
    w.field("transparent", p.output.transparent);
    w.field("codec", p.output.codec == Codec::H265 ? "h265" : "h264");
    w.field("quality", p.output.quality);
    if (!p.output.path.empty()) w.field("path", p.output.path);
    w.end();

    w.key("motion").object();
    w.field("smooth", p.motion.smooth);
    w.field("ease", p.motion.ease);
    w.field("constant_speed", p.motion.constant_speed);
    w.key("tension").raw(json_number_exact(p.motion.tension));
    w.end();

    auto fade = [&](const char* key, const Fade& f) {
        w.key(key).object();
        w.field("colour", kFadeNames[(int)f.colour]);
        w.key("seconds").raw(json_number_exact(f.seconds));
        w.end();
    };
    fade("fade_in", p.fade_in);
    fade("fade_out", p.fade_out);

    w.key("sources").array();
    for (const Source& s : p.sources) {
        w.object();
        w.field("path", s.path);
        w.key("style").object();
        w.field("points", kPointStyleNames[(int)s.style.point_style]);
        w.field("point_px", s.style.point_px);
        w.field("sphere_radius", s.style.sphere_radius);
        w.field("cameras", s.style.cameras);
        w.field("shade", s.style.shade);
        w.field("flat", s.style.flat);
        w.field("colour", s.style.colour);
        w.field("sh_degree", s.style.sh_degree);
        w.end();
        w.end();
    }
    w.end();

    w.key("shots").array();
    for (const Shot& s : p.shots) {
        w.object();
        w.key("start").raw(json_number_exact(s.start));
        w.field("source", s.source);
        w.field("transition", kTransitionNames[(int)s.transition]);
        w.key("duration").raw(json_number_exact(s.duration));
        w.end();
    }
    w.end();

    w.key("keyframes").array();
    for (size_t i = 0; i < p.keys.size(); i++) {
        const Keyframe& k = p.keys[i];
        w.object();
        w.key("time").raw(json_number_exact(k.time));
        write_vec(w, "position", k.pos, 3);
        write_vec(w, "rotation", k.rot, 4);
        if (k.aim) {
            write_vec(w, "look_at", k.target, 3);
            w.key("roll").raw(json_number_exact(k.roll));
        }
        if (k.own_lens || i == 0) {
            w.key("lens");
            write_lens(w, k.lens);
        }
        if (k.hold) w.field("hold", true);
        w.end();
    }
    w.end();
    w.end();
    return w.str();
}

RenderProject project_from_json(const std::string& text) {
    const JsonValue root = json_parse(text);
    if (!root.is_object() || get_str(root, "format") != kFormat)
        throw std::runtime_error("not a render project");
    RenderProject p;
    read_vec(root, "up", p.up, 3);
    read_vecf(root, "background", p.background, 3);
    p.end = std::max(0.0, get_num(root, "end", 0.0));
    {
        double a[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (read_vec(root, "placement", a, 12)) p.placement = spirula::Sim3::from_3x4(a);
    }

    if (const JsonValue* o = root.find("output"); o && o->is_object()) {
        Output& out = p.output;
        out.kind = (OutputKind)name_index(kOutputNames, get_str(*o, "kind"), 1);
        out.width = std::clamp((int)get_num(*o, "width", out.width), 16, 16384);
        out.height = std::clamp((int)get_num(*o, "height", out.height), 16, 16384);
        out.fps = std::clamp(get_num(*o, "fps", out.fps), 1.0, 240.0);
        out.format = get_str(*o, "image_format") == "jpeg" ? ImageFormat::Jpeg
                                                           : ImageFormat::Png;
        out.jpeg_quality = std::clamp((int)get_num(*o, "jpeg_quality", 95), 10, 100);
        out.transparent = get_bool(*o, "transparent", false);
        out.codec = get_str(*o, "codec") == "h265" ? Codec::H265 : Codec::H264;
        out.quality = std::clamp((int)get_num(*o, "quality", 1), 0, 2);
        out.path = get_str(*o, "path");
    }
    if (const JsonValue* o = root.find("motion"); o && o->is_object()) {
        p.motion.smooth = get_bool(*o, "smooth", true);
        p.motion.ease = get_bool(*o, "ease", true);
        p.motion.constant_speed = get_bool(*o, "constant_speed", false);
        p.motion.tension = std::clamp(get_num(*o, "tension", 0.0), 0.0, 1.0);
    }
    auto fade = [&](const char* key, Fade& f) {
        const JsonValue* o = root.find(key);
        if (!o || !o->is_object()) return;
        f.colour = (FadeColour)name_index(kFadeNames, get_str(*o, "colour"), 0);
        f.seconds = std::clamp(get_num(*o, "seconds", 1.0), 0.0, 3600.0);
    };
    fade("fade_in", p.fade_in);
    fade("fade_out", p.fade_out);

    if (const JsonValue* a = root.find("sources"); a && a->is_array()) {
        for (const JsonValue& o : a->arr) {
            Source s;
            s.path = get_str(o, "path");
            if (const JsonValue* st = o.find("style"); st && st->is_object()) {
                SourceStyle& y = s.style;
                y.point_style = (PointStyle)name_index(kPointStyleNames,
                                                       get_str(*st, "points"), 1);
                y.point_px = (float)std::clamp(get_num(*st, "point_px", 3.0), 0.5, 64.0);
                y.sphere_radius = (float)std::clamp(get_num(*st, "sphere_radius", 0.004),
                                                    1e-6, 1.0);
                y.cameras = get_bool(*st, "cameras", false);
                y.shade = get_bool(*st, "shade", true);
                y.flat = get_bool(*st, "flat", false);
                y.colour = get_bool(*st, "colour", true);
                y.sh_degree = std::clamp((int)get_num(*st, "sh_degree", -1), -1, 3);
            }
            p.sources.push_back(std::move(s));
        }
    }
    if (const JsonValue* a = root.find("shots"); a && a->is_array()) {
        for (const JsonValue& o : a->arr) {
            Shot s;
            s.start = std::max(0.0, get_num(o, "start", 0.0));
            s.source = std::max(-1, (int)get_num(o, "source", 0));
            s.transition = (Transition)name_index(kTransitionNames,
                                                  get_str(o, "transition"), 0);
            s.duration = std::clamp(get_num(o, "duration", 1.0), 0.0, 3600.0);
            p.shots.push_back(s);
        }
        std::stable_sort(p.shots.begin(), p.shots.end(),
                         [](const Shot& a, const Shot& b) { return a.start < b.start; });
    }
    if (const JsonValue* a = root.find("keyframes"); a && a->is_array()) {
        for (const JsonValue& o : a->arr) {
            Keyframe k;
            k.time = std::max(0.0, get_num(o, "time", 0.0));
            read_vec(o, "position", k.pos, 3);
            if (read_vec(o, "rotation", k.rot, 4)) {
                const double n = std::sqrt(k.rot[0]*k.rot[0] + k.rot[1]*k.rot[1] +
                                           k.rot[2]*k.rot[2] + k.rot[3]*k.rot[3]);
                if (n > 1e-12) for (double& v : k.rot) v /= n;
                else { k.rot[0] = 1; k.rot[1] = k.rot[2] = k.rot[3] = 0; }
            }
            k.aim = read_vec(o, "look_at", k.target, 3);
            k.roll = get_num(o, "roll", 0.0);
            if (const JsonValue* l = o.find("lens"); l && l->is_object()) {
                k.own_lens = true;
                k.lens = read_lens(*l);
            }
            k.hold = get_bool(o, "hold", false);
            update_aim(k, p.up);
            p.keys.push_back(k);
        }
    }
    p.sort_keys();
    return p;
}

RenderProject load_project(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return project_from_json(text);
}

void save_project(const RenderProject& p, const std::string& path) {
    const fs::path target = fs::u8path(path);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    // Written beside and renamed over, so a full disk cannot leave half a file
    // where the last good project was.
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + path);
        f << project_to_json(p);
        if (!f) throw std::runtime_error("cannot write " + path);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
        if (ec) throw std::runtime_error("cannot write " + path + ": " + ec.message());
    }
}

std::string default_project_dir(const std::string& model_path) {
    std::error_code ec;
    fs::path p = fs::u8path(model_path);
    if (p.empty()) return {};
    // Projects already kept with this very file win: a model saved moved has
    // its moved copies there.
    if (fs::is_regular_file(p, ec) && fs::is_directory(p.parent_path() / "renders", ec))
        return (p.parent_path() / "renders").string();
    // A reconstruction: <dataset>/sparse/0, or the dataset folder itself.
    if (fs::is_directory(p, ec)) {
        if (p.parent_path().filename() == "sparse") return (p.parent_path().parent_path() / "renders").string();
        if (p.filename() == "sparse") return (p.parent_path() / "renders").string();
        return (p / "renders").string();
    }
    // A model from a run: the run's config.json names the dataset it read.
    for (fs::path dir = p.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const fs::path cfg = dir / "config.json";
        if (fs::is_regular_file(cfg, ec)) {
            try {
                const JsonValue v = json_parse_file(cfg.string());
                const JsonValue* d = v.find("data");
                if (d && !d->as_string().empty()) {
                    fs::path data = fs::u8path(d->as_string());
                    if (data.is_relative()) data = dir / data;
                    if (data.filename() == "images") data = data.parent_path();
                    if (fs::is_directory(data, ec)) return (data / "renders").string();
                }
            } catch (const std::exception&) {
            }
            break;
        }
        if (dir == p.parent_path().parent_path()) break;
    }
    return (p.parent_path() / "renders").string();
}

}  // namespace gui::render

namespace gui::render {

int copy_moved_projects(const std::string& from_model, const std::string& to_model,
                        const spirula::Sim3& move, std::string& dir) {
    if (move.is_identity(1e-9)) return 0;
    std::error_code ec;
    const fs::path src = fs::u8path(default_project_dir(from_model));
    fs::path to = fs::u8path(to_model);
    if (!fs::is_directory(to, ec)) to = to.parent_path();
    const fs::path dst = to / "renders";
    if (!fs::is_directory(src, ec)) return 0;
    int n = 0;
    for (const auto& e : fs::directory_iterator(src, ec)) {
        if (e.path().extension() != ".json") continue;
        try {
            RenderProject p = load_project(e.path().string());
            // Its poses are in the placement it was laid out against; the new
            // file's coordinates are the original's moved by `move`.
            transform_project(p, move * p.placement.inverse());
            p.placement = spirula::Sim3();
            fs::path out = dst / e.path().filename();
            // Never over an original: the same folder gets a new name.
            if (fs::equivalent(src, dst, ec))
                out = dst / (e.path().stem().string() + "_moved.json");
            save_project(p, out.string());
            n++;
        } catch (const std::exception&) {
        }
    }
    if (n) dir = dst.string();
    return n;
}

}  // namespace gui::render
