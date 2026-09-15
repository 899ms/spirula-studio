// Moving a detection's boundary before it joins the mask -- outward or inward,
// see MaskOptions::dilate_ratio.
//
// Separate from Masking.cpp because none of it needs a model, a device or a
// session: it is geometry over one binary mask, and a test binary can run it
// on a machine with no GPU.

#include "sam/Masking.h"

#include "core/DistanceTransform.h"
#include "nn/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sam {

namespace {

// The union without a margin, and the path `dilate_ratio = 0` must keep taking
// byte for byte.
void or_into(const Mask& m, std::vector<uint8_t>& hit) {
    const uint8_t* src = m.data.data();
    uint8_t* dst = hit.data();
    nn::parallel_for((int64_t)m.data.size(), [src, dst](int64_t lo, int64_t hi) {
        for (int64_t i = lo; i < hi; ++i)
            if (src[i] > 127) dst[i] = 1;
    }, /*min_chunk=*/65536);
}

}  // namespace

int dilate_radius_px(const Box& box, float dilate_ratio) {
    if (dilate_ratio == 0.0f) return 0;
    const float bw = std::max(1.0f, box.x1 - box.x0);
    const float bh = std::max(1.0f, box.y1 - box.y0);
    // The odd kernel SIZE first, then the radius as half of it: moving the
    // boundary by r changes the box by 2r, so it gains or loses exactly
    // `dilate_ratio` of its mean side. Floor of 3 keeps a tiny detection moving.
    int k = (int)(std::fabs(dilate_ratio) * 0.5f * (bw + bh));
    if (k < 3) k = 3;
    k |= 1;
    const int r = (k - 1) / 2;
    return dilate_ratio > 0.0f ? r : -r;
}

void accumulate_dilated(const Mask& m, int radius, std::vector<uint8_t>& hit) {
    if (m.data.empty() || m.data.size() != hit.size()) return;
    // A mask whose dimensions do not describe its bytes cannot be cropped, and
    // the union is still well defined; take it flat rather than drop it.
    const bool shaped = (size_t)m.width * (size_t)m.height == m.data.size();
    if (radius == 0 || !shaped) {
        or_into(m, hit);
        return;
    }

    int x0 = m.width, y0 = m.height, x1 = -1, y1 = -1;
    for (int y = 0; y < m.height; ++y) {
        const uint8_t* row = m.data.data() + (size_t)y * m.width;
        int rx0 = -1, rx1 = -1;
        for (int x = 0; x < m.width; ++x)
            if (row[x] > 127) { if (rx0 < 0) rx0 = x; rx1 = x; }
        if (rx0 < 0) continue;
        if (rx0 < x0) x0 = rx0;
        if (rx1 > x1) x1 = rx1;
        if (y < y0) y0 = y;
        y1 = y;
    }
    if (x1 < 0) return;

    // The bounding box plus the offset holds the whole answer. Outward, the
    // clip to the frame IS the border rule; inward the crop runs past it and
    // repeats the border pixel, so a subject the frame cuts off keeps that edge.
    const int pad = radius < 0 ? -radius : radius;
    const int cx0 = radius > 0 ? std::max(0, x0 - pad) : x0 - pad;
    const int cy0 = radius > 0 ? std::max(0, y0 - pad) : y0 - pad;
    const int cx1 = radius > 0 ? std::min(m.width - 1, x1 + pad) : x1 + pad;
    const int cy1 = radius > 0 ? std::min(m.height - 1, y1 + pad) : y1 + pad;
    const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;

    std::vector<uint8_t> crop((size_t)cw * ch);
    for (int y = 0; y < ch; ++y) {
        const int sy = std::clamp(cy0 + y, 0, m.height - 1);
        const uint8_t* src = m.data.data() + (size_t)sy * m.width;
        uint8_t* dst = crop.data() + (size_t)y * cw;
        for (int x = 0; x < cw; ++x)
            dst[x] = src[std::clamp(cx0 + x, 0, m.width - 1)] > 127 ? 1 : 0;
    }
    edt::apply_mask_boundary_offset_in_place(crop.data(), ch, cw, (float)radius);
    for (int y = 0; y < ch; ++y) {
        const int dy = cy0 + y;
        if (dy < 0 || dy >= m.height) continue;
        const uint8_t* src = crop.data() + (size_t)y * cw;
        uint8_t* dst = hit.data() + (size_t)dy * m.width;
        for (int x = 0; x < cw; ++x) {
            const int dx = cx0 + x;
            if (dx >= 0 && dx < m.width && src[x]) dst[dx] = 1;
        }
    }
}

void compose_hit(const Result& positive, const Result& negative,
                 float dilate_ratio, std::vector<uint8_t>& hit) {
    for (const Detection& d : positive.detections) {
        if (d.mask.data.size() != hit.size()) continue;
        accumulate_dilated(d.mask, dilate_radius_px(d.box, dilate_ratio), hit);
    }
    // After, never before: the margin is a guess about where the object really
    // ends, and a negative phrase is the user saying it does not end there.
    for (const Detection& d : negative.detections) {
        if (d.mask.data.size() != hit.size()) continue;
        const uint8_t* src = d.mask.data.data();
        uint8_t* dst = hit.data();
        nn::parallel_for((int64_t)hit.size(), [src, dst](int64_t lo, int64_t hi) {
            for (int64_t i = lo; i < hi; ++i)
                if (src[i] > 127) dst[i] = 0;
        }, /*min_chunk=*/65536);
    }
}

}  // namespace sam
