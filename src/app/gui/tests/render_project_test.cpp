// render_project_test -- the render mode's pure half (app/gui/render/): the
// trajectory through its keys, holds, eased ends and constant speed; the
// lens arithmetic; a project's JSON round trip; and moved-project copies.

#include "app/gui/render/LensPresets.h"
#include "app/gui/render/RenderProject.h"
#include "app/gui/render/Trajectory.h"
#include "data/DatasetParser.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace gui::render;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
}

// |dot| of two unit quaternions: 1 when they are the same rotation.
double qsame(const double a[4], const double b[4]) {
    return std::fabs(a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]);
}

Keyframe key(double t, double x, double y, double z) {
    Keyframe k;
    k.time = t;
    k.pos[0] = x;
    k.pos[1] = y;
    k.pos[2] = z;
    return k;
}

RenderProject square() {
    RenderProject p;
    p.keys = {key(0, 0, 0, 0), key(1, 1, 0, 0), key(3, 1, 1, 0), key(4, 0, 1, 0)};
    const double target[3] = {0.5, 0.5, -1.0};
    for (Keyframe& k : p.keys) {
        k.aim = true;
        for (int d = 0; d < 3; d++) k.target[d] = target[d];
        update_aim(k, p.up);
    }
    p.keys[0].own_lens = true;
    return p;
}

void test_keys_are_hit() {
    RenderProject p = square();
    p.keys[2].hold = true;
    const Trajectory tr(p);
    bool through = true;
    for (const Keyframe& k : p.keys) {
        const CameraState c = tr.at(k.time);
        through = through && dist3(c.pos, k.pos) < 1e-9 && qsame(c.rot, k.rot) > 1 - 1e-9;
    }
    check(through, "the path passes through every key, position and rotation");

    // Eased ends and a hold: no motion across a tiny step at each.
    auto speed = [&](double t) {
        const CameraState a = tr.at(t - 1e-4), b = tr.at(t + 1e-4);
        return dist3(a.pos, b.pos) / 2e-4;
    };
    const CameraState s0 = tr.at(0.0), s1 = tr.at(1e-4);
    check(dist3(s0.pos, s1.pos) / 1e-4 < 1e-2, "an eased start starts from rest");
    check(speed(3.0) < 1e-2, "a hold stops the camera");
    check(speed(2.0) > 0.1, "between keys it moves");

    p.motion.ease = false;
    const Trajectory lin(p);
    const CameraState e0 = lin.at(0.0), e1 = lin.at(1e-4);
    check(dist3(e0.pos, e1.pos) / 1e-4 > 0.5, "without easing it leaves at speed");
}

void test_constant_speed() {
    RenderProject p;
    // Uneven spacing: a short hop over one second, a long run over another.
    p.keys = {key(0, 0, 0, 0), key(1, 0.1, 0, 0), key(2, 5, 0, 0)};
    p.keys[0].own_lens = true;
    p.motion.constant_speed = true;
    p.motion.ease = false;
    const Trajectory tr(p);
    double lo = 1e30, hi = 0.0;
    for (int i = 1; i < 19; i++) {
        const double t = 2.0 * i / 20.0;
        const CameraState a = tr.at(t), b = tr.at(t + 0.01);
        const double v = dist3(a.pos, b.pos) / 0.01;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    check(hi / lo < 1.05, "constant speed holds the speed along the path within 5%");
    const CameraState end = tr.at(2.0);
    check(std::fabs(end.pos[0] - 5.0) < 1e-6, "constant speed still ends on the last key");
}

void test_aim_and_lens() {
    RenderProject p = square();
    const Trajectory tr(p);
    // An aimed camera looks down -z of its own frame at the target.
    bool aimed = true;
    for (double t = 0.0; t <= 4.0; t += 0.25) {
        const CameraState c = tr.at(t);
        double R[9];
        quat_to_matrix3(c.rot, R);
        double f[3] = {-R[2], -R[5], -R[8]};
        double to[3] = {0.5 - c.pos[0], 0.5 - c.pos[1], -1.0 - c.pos[2]};
        const double n = std::sqrt(to[0]*to[0] + to[1]*to[1] + to[2]*to[2]);
        aimed = aimed && (f[0]*to[0] + f[1]*to[1] + f[2]*to[2]) / n > 1.0 - 1e-6;
    }
    check(aimed, "an aimed move keeps its target in the middle of the picture");

    Keyframe k = p.keys[1];
    k.roll = 30.0;
    update_aim(k, p.up);
    check(std::fabs(roll_of(k.rot, k.pos, k.target, p.up) - 30.0) < 1e-6,
          "roll_of reads back the roll aim_rotation was given");

    Lens l;
    bool fov_ok = true;
    for (Projection pr : {Projection::Perspective, Projection::Fisheye, Projection::Equisolid})
        for (double fov : {30.0, 90.0, 150.0}) {
            l.projection = pr;
            lens_set_fov(l, fov);
            fov_ok = fov_ok && std::fabs(lens_fov(l) - fov) < 1e-6;
        }
    check(fov_ok, "field of view round-trips through the focal ratio, every model");
    l.projection = Projection::Perspective;
    lens_set_fov(l, 2.0 * std::atan(18.0 / 50.0) * 180.0 / 3.14159265358979);
    check(std::fabs(lens_mm(l) - 50.0) < 1e-6, "a 50 mm lens is 50 mm");

    // A zoom glides in log focal length from one key's lens to the next.
    RenderProject z;
    z.keys = {key(0, 0, 0, 0), key(2, 0, 0, 0)};
    z.keys[0].own_lens = true;
    z.keys[0].lens.focal = 1.0;
    z.keys[1].own_lens = true;
    z.keys[1].lens.focal = 4.0;
    z.motion.ease = false;
    const Trajectory zt(z);
    check(std::fabs(zt.at(1.0).lens.focal - 2.0) < 0.2, "halfway through a zoom is near the geometric mean");
}

void test_json_and_moves() {
    RenderProject p = square();
    p.keys[1].own_lens = true;
    p.keys[1].lens.projection = Projection::Fisheye;
    p.keys[1].lens.tier = 2;
    p.keys[1].lens.dist[0] = 0.05f;
    p.keys[2].hold = true;
    p.shots = {{0.0, 0, Transition::Crossfade, 0.5}, {2.0, 1, Transition::Sweep, 1.5}};
    p.sources = {{"a.ply", {}}, {"b.ply", {}}};
    p.sources[1].style.point_style = PointStyle::Sphere;
    p.fade_out.colour = FadeColour::White;
    p.output.kind = OutputKind::Frames;
    p.output.codec = Codec::H265;
    const RenderProject q = project_from_json(project_to_json(p));
    bool same = q.keys.size() == p.keys.size();
    for (size_t i = 0; same && i < p.keys.size(); i++)
        same = dist3(q.keys[i].pos, p.keys[i].pos) < 1e-12 && q.keys[i].aim == p.keys[i].aim &&
               q.keys[i].hold == p.keys[i].hold && (i == 0 || q.keys[i].own_lens == p.keys[i].own_lens);
    same = same && q.keys[1].lens == p.keys[1].lens && q.shots.size() == 2 &&
           q.shots[1].transition == Transition::Sweep && q.sources[1].style.point_style == PointStyle::Sphere &&
           q.fade_out.colour == FadeColour::White && q.output.kind == OutputKind::Frames &&
           q.output.codec == Codec::H265;
    check(same, "a project survives its JSON");
    check(project_to_json(q) == project_to_json(p), "and writes back byte for byte");

    // Turning the whole path keeps every camera aimed at the turned target.
    const double axis[3] = {0, 0, 1}, c[3] = {0, 0, 0};
    const spirula::Sim3 turn = spirula::Sim3::rotation_about(axis, 0.7, c);
    RenderProject t = p;
    transform_project(t, turn);
    const Trajectory a(p), b(t);
    bool rigid = true;
    for (double s = 0.0; s <= 4.0; s += 0.5) {
        const CameraState x = a.at(s), y = b.at(s);
        double moved[3];
        turn.apply(x.pos, moved);
        rigid = rigid && dist3(moved, y.pos) < 1e-9;
    }
    check(rigid, "transform_project moves the whole path rigidly");

    // A model saved moved gets moved copies beside it; the originals stay.
    const fs::path dir = fs::temp_directory_path() / "spirula_render_project_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "old" / "renders");
    fs::create_directories(dir / "new");
    { std::FILE* f = std::fopen((dir / "old" / "model.ply").string().c_str(), "wb"); std::fclose(f); }
    { std::FILE* f = std::fopen((dir / "new" / "model.ply").string().c_str(), "wb"); std::fclose(f); }
    save_project(p, (dir / "old" / "renders" / "shot.json").string());
    std::string where;
    const int n = copy_moved_projects((dir / "old" / "model.ply").string(),
                                      (dir / "new" / "model.ply").string(), turn, where);
    const RenderProject moved = load_project((dir / "new" / "renders" / "shot.json").string());
    const RenderProject kept = load_project((dir / "old" / "renders" / "shot.json").string());
    double m[3];
    turn.apply(p.keys[2].pos, m);
    check(n == 1 && dist3(moved.keys[2].pos, m) < 1e-9 && dist3(kept.keys[2].pos, p.keys[2].pos) < 1e-12,
          "a moved model's projects are copied moved, the originals untouched");
    check(default_project_dir((dir / "new" / "model.ply").string()) == (dir / "new" / "renders").string(),
          "the moved copies are what the moved model finds first");
    fs::remove_all(dir);
}

void test_dataset_lenses() {
    ParsedDataset ds;
    ds.num_cameras = 9;
    for (int i = 0; i < 9; i++) {
        const bool wide = i >= 6;
        ds.widths.push_back(1920);
        ds.heights.push_back(1080);
        ds.camera_models.push_back(0);
        ds.camera_distortions.push_back(0);
        const float f = (wide ? 1000.0f : 1600.0f) + (float)(i % 3);
        ds.intrins.insert(ds.intrins.end(), {f, f, 960.0f, 540.0f});
        for (int d = 0; d < 8; d++) ds.dist_coeffs.push_back(0.0f);
    }
    const std::vector<DatasetLens> l = cluster_dataset_lenses(ds);
    check(l.size() == 2 && l[0].count == 6 && l[1].count == 3 &&
              std::fabs(l[0].lens.focal * 1920.0 - 1601.0) < 0.01,
          "a zoom's two ends come out as two lenses, the common one first");
}

}  // namespace

int main() {
    test_keys_are_hit();
    test_constant_speed();
    test_aim_and_lens();
    test_json_and_moves();
    test_dataset_lenses();
    if (g_failures) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
