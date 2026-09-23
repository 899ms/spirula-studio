// Picture.cpp -- see Picture.h.

#include "app/gui/Picture.h"

#include "app/DepthColor.h"
#include "app/FrameLook.h"          // app::photo_turn
#include "app/FrameMask.h"          // app::load_rgb, app::load_stencil
#include "core/ExrImage.h"
#include "external/stb_image.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace gui {

namespace {

// The masked-out region, tinted the way the mask preview tints it so that the
// two read as one answer.
void tint(uint8_t* px, int r, int g, int b) {
    px[0] = (uint8_t)(r / 3 + 150);
    px[1] = (uint8_t)(g / 3);
    px[2] = (uint8_t)(b / 3);
}

// Area average to an exact size. The row's panels arrive at three different
// resolutions and have to line up before they can be laid side by side.
void scale_to(const uint8_t* src, int sw, int sh, int dw, int dh,
              std::vector<uint8_t>& dst) {
    dst.assign((size_t)dw * dh * 3, 0);
    for (int y = 0; y < dh; y++) {
        const int sy0 = (int)((int64_t)y * sh / dh);
        const int sy1 = std::max(sy0 + 1, (int)((int64_t)(y + 1) * sh / dh));
        for (int x = 0; x < dw; x++) {
            const int sx0 = (int)((int64_t)x * sw / dw);
            const int sx1 = std::max(sx0 + 1, (int)((int64_t)(x + 1) * sw / dw));
            int acc[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++)
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    const uint8_t* p = &src[((size_t)sy * sw + sx) * 3];
                    for (int c = 0; c < 3; c++) acc[c] += p[c];
                    n++;
                }
            if (!n) continue;
            uint8_t* d = &dst[((size_t)y * dw + x) * 3];
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)(acc[c] / n);
        }
    }
}

// A 16-bit depth PNG through the viewport's ramp. Read here rather than
// through app::load_rgb, which would hand back the high byte as grey.
bool load_depth_rgb(const std::string& path, int& w, int& h,
                    std::vector<uint8_t>& rgb) {
    int ch = 0;
    stbi_us* img = stbi_load_16(path.c_str(), &w, &h, &ch, 1);
    if (!img) return false;
    const size_t n = (size_t)w * h;
    std::vector<float> d(n);
    for (size_t i = 0; i < n; i++) d[i] = (float)img[i];
    stbi_image_free(img);
    rgb.resize(n * 3);
    // 0 is the trainer's "no ground truth here" and must not set the range.
    app::depth_to_rgb(d.data(), n, /*skip_zero=*/true, rgb.data());
    return true;
}

// stb's buffer, freed however the scope is left: a resize that throws must not
// strand an 84 MB decode.
struct StbFree {
    void operator()(unsigned char* p) const { stbi_image_free(p); }
};
using StbPixels = std::unique_ptr<unsigned char, StbFree>;

// Sizes `out` for a w x h source at `max_side` without freeing its buffer, and
// returns the box step: whole source pixels, never upscaling.
int size_picture(int w, int h, int max_side, Picture& out) {
    const int step = max_side > 0
                         ? std::max(1, (std::max(w, h) + max_side - 1) / max_side)
                         : 1;
    out.w = std::max(1, w / step);
    out.h = std::max(1, h / step);
    out.rgb.resize((size_t)out.w * out.h * 3);
    out.src_w = w;
    out.src_h = h;
    // Full resolution answers every pane there is.
    out.made_for = max_side > 0 ? max_side : INT_MAX;
    return step;
}

// Box average over the step x step source block: a point sample of a 4K
// frame decimated 16x aliases into noise, which reads as a bad mask.
void box_photo(const uint8_t* rgb, int w, int h, int step, Picture& out) {
    if (step == 1) {
        std::copy(rgb, rgb + (size_t)w * h * 3, out.rgb.begin());
        return;
    }
    for (int y = 0; y < out.h; y++) {
        const int sy1 = std::min(h, (y + 1) * step);
        for (int x = 0; x < out.w; x++) {
            const int sx1 = std::min(w, (x + 1) * step);
            int acc[3] = {0, 0, 0};
            int n = 0;
            for (int sy = y * step; sy < sy1; sy++) {
                const uint8_t* p = &rgb[((size_t)sy * w + x * step) * 3];
                for (int sx = x * step; sx < sx1; sx++, n++, p += 3) {
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            }
            uint8_t* px = &out.rgb[((size_t)y * out.w + x) * 3];
            for (int c = 0; c < 3; c++) px[c] = (uint8_t)(n ? acc[c] / n : 0);
        }
    }
}

// Tints each of `out`'s blocks in which fewer than half the source pixels are
// kept. A mask of another size than its w x h image is sampled nearest: a
// mask that came with the capture rather than one the run made.
void tint_blocks(const uint8_t* mask, int mw, int mh, int w, int h, int step,
                 bool mask_flipped, Picture& out) {
    const bool same = mw == w && mh == h;
    if (same && step == 1) {
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t* px = &out.rgb[i * 3];
            if ((mask[i] > 127) == mask_flipped) tint(px, px[0], px[1], px[2]);
        }
        return;
    }
    for (int y = 0; y < out.h; y++) {
        const int sy1 = std::min(h, (y + 1) * step);
        for (int x = 0; x < out.w; x++) {
            const int sx1 = std::min(w, (x + 1) * step);
            int n = 0, keep = 0;
            for (int sy = y * step; sy < sy1; sy++) {
                const int my = same ? sy : std::min(mh - 1, sy * mh / h);
                const uint8_t* row = &mask[(size_t)my * mw];
                for (int sx = x * step; sx < sx1; sx++, n++)
                    keep += row[same ? sx : std::min(mw - 1, sx * mw / w)] > 127;
            }
            if (mask_flipped) keep = n - keep;
            if (keep * 2 < n) {
                uint8_t* px = &out.rgb[((size_t)y * out.w + x) * 3];
                tint(px, px[0], px[1], px[2]);
            }
        }
    }
}

}  // namespace

void make_picture(const uint8_t* rgb, int w, int h, const uint8_t* mask,
                  int max_side, Picture& out) {
    out = Picture{};
    if (!rgb || w <= 0 || h <= 0) return;
    const int step = size_picture(w, h, max_side, out);
    box_photo(rgb, w, h, step, out);
    if (mask) tint_blocks(mask, w, h, w, h, step, false, out);
}

bool load_picture(const std::string& image_path, const std::string& mask_path,
                  int max_side, Picture& out, bool mask_flipped) {
    const auto fail = [&out] {
        out.rgb.clear();
        out.w = out.h = out.src_w = out.src_h = out.made_for = 0;
        return false;
    };
    if (image_path.empty()) return fail();
    int w = 0, h = 0, comp = 0, step = 1;
    if (exr::is_exr(image_path)) {
        std::vector<uint8_t> rgb;
        if (!app::load_rgb(image_path, w, h, rgb) || w <= 0 || h <= 0) return fail();
        step = size_picture(w, h, max_side, out);
        box_photo(rgb.data(), w, h, step, out);
    } else {
        const StbPixels rgb(stbi_load(image_path.c_str(), &w, &h, &comp, 3));
        if (!rgb || w <= 0 || h <= 0) return fail();
        step = size_picture(w, h, max_side, out);
        box_photo(rgb.get(), w, h, step, out);
    }
    if (mask_path.empty()) return true;

    // stb's buffer in place, unless the mask needs what load_stencil adds: an
    // EXR decode, or the EXIF turn a JPEG mask may carry.
    int mw = 0, mh = 0;
    StbPixels m(exr::is_exr(mask_path) ? nullptr
                                       : stbi_load(mask_path.c_str(), &mw, &mh, &comp, 1));
    if (m && app::photo_turn(mask_path).identity()) {
        tint_blocks(m.get(), mw, mh, w, h, step, mask_flipped, out);
        return true;
    }
    m.reset();
    std::vector<uint8_t> stencil;
    if (app::load_stencil(mask_path, mw, mh, stencil))
        tint_blocks(stencil.data(), mw, mh, w, h, step, mask_flipped, out);
    return true;
}

bool load_picture_row(const std::vector<PicturePanel>& panels, int max_side,
                      Picture& out) {
    out = Picture{};
    struct Loaded { int w = 0, h = 0; std::vector<uint8_t> rgb; };
    std::vector<Loaded> got;
    double aspect_sum = 0.0;
    int tallest = 0;
    for (const PicturePanel& p : panels) {
        Loaded l;
        const bool ok = p.depth ? load_depth_rgb(p.path, l.w, l.h, l.rgb)
                                : app::load_rgb(p.path, l.w, l.h, l.rgb);
        if (!ok || l.w <= 0 || l.h <= 0) continue;
        aspect_sum += (double)l.w / (double)l.h;
        tallest = std::max(tallest, l.h);
        got.push_back(std::move(l));
    }
    if (got.empty() || aspect_sum <= 0.0) return false;

    // The row's LONG edge is what the pane budgets, and that edge is its
    // width: at a common height h the row is h * sum(aspect) wide.
    int h0 = tallest;
    if (max_side > 0)
        h0 = std::clamp((int)std::lround(max_side / aspect_sum), 32, tallest);

    std::vector<int> widths(got.size());
    int total = 0;
    for (size_t i = 0; i < got.size(); i++) {
        widths[i] = std::max(1, (int)std::lround(
                                    (double)h0 * got[i].w / got[i].h));
        total += widths[i];
    }

    std::vector<uint8_t> row((size_t)total * h0 * 3, 0);
    std::vector<uint8_t> panel;
    int x0 = 0;
    for (size_t i = 0; i < got.size(); i++) {
        scale_to(got[i].rgb.data(), got[i].w, got[i].h, widths[i], h0, panel);
        for (int y = 0; y < h0; y++)
            std::copy(panel.begin() + (size_t)y * widths[i] * 3,
                      panel.begin() + (size_t)(y + 1) * widths[i] * 3,
                      row.begin() + ((size_t)y * total + x0) * 3);
        x0 += widths[i];
    }
    make_picture(row.data(), total, h0, nullptr, max_side, out);
    return !out.empty();
}

}  // namespace gui
