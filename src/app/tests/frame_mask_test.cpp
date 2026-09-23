// frame_mask_test -- app/FrameMask.h shapes with no GUI: the even-odd fill
// in core/PolygonFill.h against a ray cast, the path spelling round trip,
// the path fill on a non-square frame, and the ordered composition rule.

#include "app/FrameMask.h"
#include "core/PolygonFill.h"
#include "core/SourcePath.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

// Even-odd point test by ray casting to +x, the textbook reference the
// scanline fill has to agree with at every pixel centre.
bool ray_inside(const std::vector<float>& p, float x, float y, bool& tie) {
    const size_t n = p.size() / 2;
    bool in = false;
    tie = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float ax = p[2 * i], ay = p[2 * i + 1], bx = p[2 * j], by = p[2 * j + 1];
        if ((ay > y) == (by > y)) continue;
        const float xi = ax + (y - ay) / (by - ay) * (bx - ax);
        if (std::fabs(xi - x) < 1e-3f) tie = true;
        if (x < xi) in = !in;
    }
    return in;
}

void compare_fill(const std::vector<float>& poly, int W, int H, const std::string& name) {
    std::vector<uint8_t> out((size_t)W * H, 0);
    polyfill::fill_even_odd(poly.data(), poly.size() / 2, W, H, out.data(), 1);
    size_t compared = 0, mismatched = 0, inside = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool tie;
            const bool ref = ray_inside(poly, (float)x + 0.5f, (float)y + 0.5f, tie);
            if (tie) continue;
            compared++;
            inside += ref;
            if (ref != (out[(size_t)y * W + x] != 0)) mismatched++;
        }
    check(mismatched == 0, name + ": fill matches the ray cast on " +
                               std::to_string(compared) + " pixels");
    check(compared * 10 >= (size_t)W * H * 9, name + ": at least 90% of pixels compared");
    check(inside > 0, name + ": the polygon covers something");
}

// ---------------------------------------------------------------------------
// The fill
// ---------------------------------------------------------------------------

void test_fill_matches_ray_cast() {
    // A bow tie (self-intersecting), a concave arrow, and a polygon that
    // hangs off every side of the image. Vertices sit at .3/.7 fractions so
    // no crossing lands exactly on a pixel centre.
    compare_fill({3.3f, 2.7f, 30.7f, 20.3f, 30.3f, 2.3f, 3.7f, 20.7f}, 37, 23, "bow tie");
    compare_fill({2.3f, 11.7f, 20.7f, 2.3f, 16.3f, 11.3f, 20.7f, 20.7f}, 25, 23, "arrow");
    compare_fill({-5.3f, -4.7f, 40.7f, -2.3f, 44.3f, 30.7f, -3.7f, 26.3f, 18.3f, 12.7f},
                 37, 23, "overhang");
    std::vector<uint8_t> out(37 * 23, 0);
    const std::vector<float> two = {1.3f, 1.3f, 20.7f, 20.7f};
    polyfill::fill_even_odd(two.data(), 2, 37, 23, out.data(), 1);
    size_t set = 0;
    for (uint8_t v : out) set += v;
    check(set == 0, "two points fill nothing");
    polyfill::fill_even_odd(two.data(), 0, 37, 23, out.data(), 1);
    check(true, "zero points does not crash");
}

// ---------------------------------------------------------------------------
// The spelling
// ---------------------------------------------------------------------------

bool close_to(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void test_path_spelling() {
    std::vector<app::MaskShape> s;
    std::string err;
    check(app::parse_mask_shapes("path 0.1,0.1,0.9,0.1,0.5,0.9", s, err), "parses a path");
    check(s.size() == 1 && s[0].kind == app::MaskShape::Kind::Path && !s[0].remove,
          "one keep path");
    check(s[0].pts.size() == 6 && close_to(s[0].pts[0], 0.1f) && close_to(s[0].pts[5], 0.9f),
          "six numbers in order");
    check(app::parse_mask_shapes("-path 0.1, 0.1, 0.9,0.1, 0.5,0.9", s, err) &&
              s[0].remove && s[0].pts.size() == 6,
          "-path removes, spaces after commas tolerated");
    check(app::parse_mask_shapes("!path 0,0,1,0,1,1,0,1", s, err) && s[0].remove &&
              s[0].pts.size() == 8,
          "! spelling and four corners");
    check(!app::parse_mask_shapes("path 0.1,0.1,0.9,0.1", s, err), "two points rejected");
    check(!app::parse_mask_shapes("path 0.1,0.1,0.9,0.1,0.5", s, err), "odd count rejected");
    check(!app::parse_mask_shapes("path", s, err), "no numbers rejected");
    check(!app::parse_mask_shapes("path 0.1,0.1,0.9,0.1,0.5,x", s, err), "junk rejected");
    check(!app::parse_mask_shapes("path 0.1,0.1,0.9,0.1,0.5,0.9,", s, err),
          "trailing comma rejected");

    // Mixed list, order kept, and the old kinds still spell the same.
    check(app::parse_mask_shapes(
              "ellipse 0.5,0.5,0.49,0.49; -rect 0.2,0.9,0.8,1; -path 0.1,0.1,0.3,0.1,0.2,0.3",
              s, err),
          "mixed list parses");
    check(s.size() == 3 && s[0].kind == app::MaskShape::Kind::Ellipse &&
              s[1].kind == app::MaskShape::Kind::Rect && s[2].kind == app::MaskShape::Kind::Path,
          "kinds in order");
    const std::string back = app::format_mask_shapes(s);
    check(back == "ellipse 0.5000,0.5000,0.4900,0.4900; -rect 0.2000,0.9000,0.8000,1.0000; "
                  "-path 0.1000,0.1000,0.3000,0.1000,0.2000,0.3000",
          "format spells all three: " + back);
    std::vector<app::MaskShape> again;
    check(app::parse_mask_shapes(back, again, err) && again.size() == 3 &&
              again[2].kind == app::MaskShape::Kind::Path && again[2].remove &&
              again[2].pts.size() == 6 && close_to(again[2].pts[4], 0.2f) &&
              close_to(again[2].pts[5], 0.3f),
          "format -> parse is the identity on a path");

    // A long path does not truncate: 40 corners is 80 numbers, past the 128
    // bytes the old fixed buffer held.
    app::MaskShape big;
    big.kind = app::MaskShape::Kind::Path;
    big.remove = true;
    for (int i = 0; i < 40; i++) {
        big.pts.push_back(0.5f + 0.4f * std::cos((float)i * 0.157f));
        big.pts.push_back(0.5f + 0.4f * std::sin((float)i * 0.157f));
    }
    const std::string bigs = app::format_mask_shapes({big});
    std::vector<app::MaskShape> bigp;
    check(app::parse_mask_shapes(bigs, bigp, err) && bigp.size() == 1 && bigp[0].pts.size() == 80,
          "40-corner path survives format -> parse");
    bool all = true;
    for (size_t i = 0; i < 80; i++) all &= std::fabs(bigp[0].pts[i] - big.pts[i]) < 6e-5f;
    check(all, "every corner within the %.4f rounding");
}

// ---------------------------------------------------------------------------
// The fill inside rasterize_frame_mask
// ---------------------------------------------------------------------------

app::MaskShape path_shape(std::vector<float> pts, bool remove) {
    app::MaskShape s;
    s.kind = app::MaskShape::Kind::Path;
    s.remove = remove;
    s.pts = std::move(pts);
    return s;
}

app::MaskShape rect_shape(float x0, float y0, float x1, float y1, bool remove) {
    app::MaskShape s;
    s.kind = app::MaskShape::Kind::Rect;
    s.remove = remove;
    s.cx = x0; s.cy = y0; s.rx = x1; s.ry = y1;
    return s;
}

app::MaskShape ellipse_shape(float cx, float cy, float rx, float ry, bool remove) {
    app::MaskShape s;
    s.kind = app::MaskShape::Kind::Ellipse;
    s.remove = remove;
    s.cx = cx; s.cy = cy; s.rx = rx; s.ry = ry;
    return s;
}

// The polygon in pixels of a W x H frame, for the reference test.
std::vector<float> in_pixels(const std::vector<float>& norm, int W, int H) {
    std::vector<float> px(norm.size());
    for (size_t i = 0; i + 1 < norm.size(); i += 2) {
        px[i] = norm[i] * (float)W;
        px[i + 1] = norm[i + 1] * (float)H;
    }
    return px;
}

void test_path_fill() {
    // 64 x 48: a triangle whose normalised corners give different pixel
    // polygons depending on which dimension scales which axis.
    const int W = 64, H = 48;
    const std::vector<float> tri = {0.13f, 0.11f, 0.87f, 0.21f, 0.47f, 0.93f};
    app::FrameMask m;
    m.shapes.push_back(path_shape(tri, true));
    std::vector<uint8_t> out;
    std::string err;
    check(app::rasterize_frame_mask(m, W, H, out, err), "rasterizes a -path");
    const std::vector<float> px = in_pixels(tri, W, H);
    size_t compared = 0, mismatched = 0, dropped = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool tie;
            const bool inside = ray_inside(px, (float)x + 0.5f, (float)y + 0.5f, tie);
            if (tie) continue;
            compared++;
            dropped += out[(size_t)y * W + x] == 0;
            if ((out[(size_t)y * W + x] == 0) != inside) mismatched++;
        }
    check(mismatched == 0, "-path drops exactly the ray-cast inside on 64x48");
    check(dropped > 400 && dropped < 1400, "the triangle is about a third of the frame: " +
                                             std::to_string(dropped));

    // Transposed frame: the same normalised corners on 48 x 64 give a
    // different pixel polygon, and the scaling must follow the axes.
    check(app::rasterize_frame_mask(m, H, W, out, err), "rasterizes on 48x64");
    const std::vector<float> px2 = in_pixels(tri, H, W);
    mismatched = 0;
    for (int y = 0; y < W; y++)
        for (int x = 0; x < H; x++) {
            bool tie;
            const bool inside = ray_inside(px2, (float)x + 0.5f, (float)y + 0.5f, tie);
            if (tie) continue;
            if ((out[(size_t)y * H + x] == 0) != inside) mismatched++;
        }
    check(mismatched == 0, "-path on the transposed frame still matches");

    // A keep path as the only shape: outside is 0, inside 255.
    app::FrameMask k;
    k.shapes.push_back(path_shape(tri, false));
    check(app::rasterize_frame_mask(k, W, H, out, err), "rasterizes a keep path");
    check(out[0] == 0, "outside a lone keep path is dropped");
    check(out[(size_t)(H / 2) * W + W / 2] == 255, "inside a lone keep path is kept");
}

void test_path_order() {
    // -rect over the whole frame, then a keep path: the path's inside comes
    // back (last shape wins), the rest stays dropped.
    const int W = 64, H = 48;
    app::FrameMask m;
    m.shapes.push_back(rect_shape(0.0f, 0.0f, 1.0f, 1.0f, true));
    m.shapes.push_back(path_shape({0.2f, 0.2f, 0.8f, 0.2f, 0.8f, 0.8f, 0.2f, 0.8f}, false));
    std::vector<uint8_t> out;
    std::string err;
    check(app::rasterize_frame_mask(m, W, H, out, err), "rasterizes rect then path");
    check(out[0] == 0 && out[(size_t)(H / 2) * W + W / 2] == 255,
          "keep path restores its inside over a remove rect");
    // Reverse order: the rect wins everywhere.
    std::swap(m.shapes[0], m.shapes[1]);
    check(app::rasterize_frame_mask(m, W, H, out, err), "rasterizes path then rect");
    size_t kept = 0;
    for (uint8_t v : out) kept += v != 0;
    check(kept == 0, "a remove rect after a keep path drops everything");
    // Two paths: a keep path with a -path hole.
    app::FrameMask h;
    h.shapes.push_back(path_shape({0.1f, 0.1f, 0.9f, 0.1f, 0.9f, 0.9f, 0.1f, 0.9f}, false));
    h.shapes.push_back(path_shape({0.4f, 0.4f, 0.6f, 0.4f, 0.6f, 0.6f, 0.4f, 0.6f}, true));
    check(app::rasterize_frame_mask(h, W, H, out, err), "rasterizes two paths");
    check(out[(size_t)(H / 2) * W + W / 2] == 0 && out[(size_t)(H / 4) * W + W / 4] == 255 &&
              out[0] == 0,
          "hole dropped, ring kept, outside dropped");

    // A remove-path over a keep rect: the rect keeps the frame, the path
    // carves its hole out of it.
    app::FrameMask rp;
    rp.shapes.push_back(rect_shape(0.0f, 0.0f, 1.0f, 1.0f, false));
    rp.shapes.push_back(path_shape({0.4f, 0.4f, 0.6f, 0.4f, 0.6f, 0.6f, 0.4f, 0.6f}, true));
    check(app::rasterize_frame_mask(rp, W, H, out, err), "rasterizes rect then remove-path");
    check(out[(size_t)(H / 2) * W + W / 2] == 0 && out[0] == 255,
          "remove-path bites a hole out of a keep rect");

    // A keep ellipse with a remove-path bite: outside the ellipse is already
    // dropped by the base rule, so the path only matters where they overlap.
    app::FrameMask ep;
    ep.shapes.push_back(ellipse_shape(0.5f, 0.5f, 0.45f, 0.45f, false));
    ep.shapes.push_back(path_shape({0.4f, 0.4f, 0.6f, 0.4f, 0.6f, 0.6f, 0.4f, 0.6f}, true));
    check(app::rasterize_frame_mask(ep, W, H, out, err), "rasterizes ellipse then remove-path");
    check(out[(size_t)(H / 2) * W + W / 2] == 0 && out[0] == 0 &&
              out[(size_t)7 * W + 32] == 255,
          "remove-path bite inside a keep ellipse, outside it stays dropped either way");

    // A keep ellipse restoring over a fully removed path: inside the ellipse
    // comes back, outside it stays gone.
    app::FrameMask pe;
    pe.shapes.push_back(path_shape({0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}, true));
    pe.shapes.push_back(ellipse_shape(0.5f, 0.5f, 0.45f, 0.45f, false));
    check(app::rasterize_frame_mask(pe, W, H, out, err), "rasterizes remove-path then ellipse");
    check(out[(size_t)(H / 2) * W + W / 2] == 255 && out[0] == 0,
          "keep ellipse restores its interior over a fully removed path");
}

// ---------------------------------------------------------------------------
// format_mask_shapes must not truncate at any magnitude
// ---------------------------------------------------------------------------

bool close_rel(float a, float b) { return std::fabs(a - b) <= std::fabs(a) * 1e-5f + 1e-3f; }

void test_format_extreme_values_do_not_truncate() {
    // "rect -123456789275539452985344.0000,...,1000000013848427855085568.0000"
    // needs 127 characters with the remove prefix -- 31 past a 96-byte buffer.
    app::MaskShape r;
    r.kind = app::MaskShape::Kind::Rect;
    r.remove = true;
    r.cx = -1.23456789e23f;
    r.cy = 9.87654321e22f;
    r.rx = -5.55555555e23f;
    r.ry = 1.0e24f;
    app::MaskShape e;
    e.kind = app::MaskShape::Kind::Ellipse;
    e.cx = -2.5e22f;
    e.cy = 3.5e22f;
    e.rx = 4.0e22f;
    e.ry = 6.0e22f;

    const std::string out = app::format_mask_shapes({r, e});
    std::vector<app::MaskShape> parsed;
    std::string err;
    check(app::parse_mask_shapes(out, parsed, err) && parsed.size() == 2,
          "an extreme rect and ellipse both re-parse: " + err);
    check(parsed.size() == 2 && parsed[0].kind == app::MaskShape::Kind::Rect &&
              parsed[0].remove && close_rel(parsed[0].cx, r.cx) &&
              close_rel(parsed[0].cy, r.cy) && close_rel(parsed[0].rx, r.rx) &&
              close_rel(parsed[0].ry, r.ry),
          "the rect's four extreme values survive whole, including the last one");
    check(parsed.size() == 2 && parsed[1].kind == app::MaskShape::Kind::Ellipse &&
              !parsed[1].remove && close_rel(parsed[1].cx, e.cx) &&
              close_rel(parsed[1].cy, e.cy) && close_rel(parsed[1].rx, e.rx) &&
              close_rel(parsed[1].ry, e.ry),
          "the ellipse's four extreme values survive whole");
}

// ---------------------------------------------------------------------------
// polyfill::contains must share fill_even_odd's boundary rule
// ---------------------------------------------------------------------------

// Unlike test_fill_matches_ray_cast's vertices (deliberately off the pixel
// grid, so no crossing lands on a sample point), these sit exactly on it --
// the case a strict ray cast and the fill's closed pixel span disagree on.
void compare_contains(const std::vector<float>& poly, int W, int H, const std::string& name) {
    std::vector<uint8_t> out((size_t)W * H, 0);
    polyfill::fill_even_odd(poly.data(), poly.size() / 2, W, H, out.data(), 1);
    size_t mismatched = 0, inside = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const bool ref = out[(size_t)y * W + x] != 0;
            const bool q = polyfill::contains(poly.data(), poly.size() / 2, (float)x + 0.5f,
                                              (float)y + 0.5f);
            inside += ref;
            if (ref != q) mismatched++;
        }
    check(mismatched == 0, name + ": point query matches the fill on all " +
                               std::to_string((size_t)W * H) + " pixels, " +
                               std::to_string(mismatched) + " mismatched");
    check(inside > 0, name + ": the polygon covers something");
}

void test_path_point_matches_fill_boundary() {
    // A concave pentagon and a bow tie, vertices on the pixel-centre grid,
    // on a non-square frame.
    compare_contains({10.5f, 10.5f, 90.5f, 10.5f, 90.5f, 70.5f, 50.5f, 40.5f, 10.5f, 70.5f},
                     101, 81, "concave, grid-aligned");
    compare_contains({10.5f, 10.5f, 90.5f, 70.5f, 90.5f, 10.5f, 10.5f, 70.5f}, 101, 81,
                     "bow tie, grid-aligned");
}

}  // namespace

int main() {
    test_fill_matches_ray_cast();
    test_path_spelling();
    test_path_fill();
    test_path_order();
    test_format_extreme_values_do_not_truncate();
    test_path_point_matches_fill_boundary();
    std::printf("%s: %d failure(s)\n", SS_FILE, g_failures);
    return g_failures;
}
