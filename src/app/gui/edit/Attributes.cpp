// Attributes.cpp -- see Attributes.h.

#include "app/gui/edit/Attributes.h"

#include "app/gui/edit/EditDoc.h"
#include "checkpoint/SplatPly.h"
#include "i18n/catalog/EditAttributes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace msg = spirula::i18n::msg::attr;

namespace gui {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
// The rasterizer's alpha cut (core/Common.cuh ALPHA_THRESHOLD), which is what
// decides how far from its centre a Gaussian is still drawn.
constexpr float kAlphaCut = 1.0f / 255.0f;

const AttrInfo kTable[(int)Attr::Count] = {
    {Attr::PosX, &msg::a_pos_x, &msg::a_pos_help, false, AttrTint::Red},
    {Attr::PosY, &msg::a_pos_y, &msg::a_pos_help, false, AttrTint::Green},
    {Attr::PosZ, &msg::a_pos_z, &msg::a_pos_help, false, AttrTint::Blue},
    {Attr::Opacity, &msg::a_opacity, &msg::a_opacity_help, false, AttrTint::None, 0.0, 1.0},
    {Attr::ScaleMax, &msg::a_scale_max, &msg::a_scale_help, true, AttrTint::None},
    {Attr::ScaleMin, &msg::a_scale_min, &msg::a_scale_help, true, AttrTint::None},
    {Attr::ScaleMean, &msg::a_scale_mean, &msg::a_scale_help, true, AttrTint::None},
    {Attr::ExtentMax, &msg::a_extent_max, &msg::a_extent_help, true, AttrTint::None},
    {Attr::ExtentMin, &msg::a_extent_min, &msg::a_extent_help, true, AttrTint::None},
    {Attr::ExtentMean, &msg::a_extent_mean, &msg::a_extent_help, true, AttrTint::None},
    {Attr::AnisoRatio, &msg::a_aniso_ratio, &msg::a_aniso_ratio_help, true, AttrTint::None},
    {Attr::Erank, &msg::a_erank, &msg::a_erank_help, false, AttrTint::None, 1.0, 3.0},
    {Attr::Red, &msg::a_red, &msg::a_colour_help, false, AttrTint::Red},
    {Attr::Green, &msg::a_green, &msg::a_colour_help, false, AttrTint::Green},
    {Attr::Blue, &msg::a_blue, &msg::a_colour_help, false, AttrTint::Blue},
    {Attr::Luma, &msg::a_luma, &msg::a_luma_help, false, AttrTint::Gray},
    {Attr::ChromaU, &msg::a_chroma_u, &msg::a_chroma_help, false, AttrTint::None},
    {Attr::ChromaV, &msg::a_chroma_v, &msg::a_chroma_help, false, AttrTint::None},
    {Attr::Hue, &msg::a_hue, &msg::a_hue_help, false, AttrTint::Hue, 0.0, 360.0},
    {Attr::Saturation, &msg::a_saturation, &msg::a_saturation_help, false, AttrTint::None},
    {Attr::CameraDistance, &msg::a_camera_distance, &msg::a_camera_distance_help, true,
     AttrTint::None},
};

bool is_colour(Attr a) { return a >= Attr::Red && a <= Attr::Saturation; }
bool is_shape(Attr a) { return a >= Attr::Opacity && a <= Attr::Erank; }

float colour_scalar(Attr a, const float* c) {
    const float r = c[0], g = c[1], b = c[2];
    // BT.709 luma and the colour differences that go with it.
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    switch (a) {
        case Attr::Red:   return r;
        case Attr::Green: return g;
        case Attr::Blue:  return b;
        case Attr::Luma:  return y;
        case Attr::ChromaU: return (b - y) / 1.8556f;
        case Attr::ChromaV: return (r - y) / 1.5748f;
        default: break;
    }
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const float d = mx - mn;
    if (a == Attr::Saturation) return mx > 1e-6f ? d / mx : 0.0f;
    // Hue in degrees. A grey has none, and saying 0 would file every grey
    // under red.
    if (d < 1e-4f * std::max(mx, 1e-3f)) return kNaN;
    float h = mx == r ? (g - b) / d : mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d;
    h *= 60.0f;
    return h < 0 ? h + 360.0f : h;
}

void srgb_to_oklab(const float* c, float w_l, float out[3]) {
    float lin[3];
    for (int k = 0; k < 3; k++) {
        const float a = std::fabs(c[k]);
        const float v = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
        lin[k] = c[k] < 0 ? -v : v;
    }
    const float l = std::cbrt(0.4122214708f*lin[0] + 0.5363325363f*lin[1] + 0.0514459929f*lin[2]);
    const float m = std::cbrt(0.2119034982f*lin[0] + 0.6806995451f*lin[1] + 0.1073969566f*lin[2]);
    const float s = std::cbrt(0.0883024619f*lin[0] + 0.2817188376f*lin[1] + 0.6299787005f*lin[2]);
    out[0] = w_l * (0.2104542553f*l + 0.7936177850f*m - 0.0040720468f*s);
    out[1] = 1.9779984951f*l - 2.4285922050f*m + 0.4505937099f*s;
    out[2] = 0.0259040371f*l + 0.7827717662f*m - 0.8086757660f*s;
}

}  // namespace


const AttrInfo& attr_info(Attr a) { return kTable[(int)a]; }

std::vector<Attr> attributes_of(const EditDoc& doc) {
    std::vector<Attr> out;
    const bool splats = doc.splats() != nullptr;
    const bool colour = doc.colours_available();
    if (splats)
        for (Attr a : {Attr::Opacity, Attr::ExtentMax, Attr::ExtentMean,
                       Attr::ExtentMin, Attr::ScaleMax, Attr::ScaleMean,
                       Attr::ScaleMin, Attr::AnisoRatio, Attr::Erank})
            out.push_back(a);
    if (colour)
        for (Attr a : {Attr::Luma, Attr::Hue, Attr::Saturation, Attr::Red,
                       Attr::Green, Attr::Blue, Attr::ChromaU, Attr::ChromaV})
            out.push_back(a);
    if (!splats && !doc.camera_centres().empty() && doc.layer() == 0)
        out.push_back(Attr::CameraDistance);
    for (Attr a : {Attr::PosZ, Attr::PosX, Attr::PosY}) out.push_back(a);
    return out;
}

bool attribute_values(const EditDoc& doc, Attr a, std::vector<float>& out) {
    const int64_t n = doc.count();
    out.assign((size_t)n, kNaN);

    if (a <= Attr::PosZ) {
        // positions() frame -> the file's, with the placement on top.
        const spirula::Sim3 to_saved = doc.view_frame().inverse() * doc.placement();
        const float* p = doc.positions();
        const int axis = (int)a;
#pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < n; i++) {
            const double q[3] = {p[i*3], p[i*3+1], p[i*3+2]};
            double w[3];
            to_saved.apply(q, w);
            out[(size_t)i] = (float)w[axis];
        }
        return true;
    }

    if (is_shape(a)) {
        const spirula::SplatCloud* c = doc.splats();
        if (!c || c->num != n) return false;
        const float grow = (float)std::log(doc.file_placement().s);
#pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < n; i++) {
            const float* s = &c->scales[(size_t)i * 3];
            const float hi = std::max(s[0], std::max(s[1], s[2])) + grow;
            const float lo = std::min(s[0], std::min(s[1], s[2])) + grow;
            const float mean = (s[0] + s[1] + s[2]) / 3.0f + grow;
            const float op = 1.0f / (1.0f + std::exp(-c->opacities[(size_t)i]));
            // Zero for a Gaussian the rasterizer never draws at all.
            const float reach = op > kAlphaCut
                ? std::sqrt(2.0f * std::log(op / kAlphaCut)) : 0.0f;
            float v = kNaN;
            switch (a) {
                case Attr::Opacity:    v = op; break;
                case Attr::ScaleMax:   v = std::exp(hi); break;
                case Attr::ScaleMin:   v = std::exp(lo); break;
                case Attr::ScaleMean:  v = std::exp(mean); break;
                case Attr::ExtentMax:  v = std::exp(hi) * reach; break;
                case Attr::ExtentMin:  v = std::exp(lo) * reach; break;
                case Attr::ExtentMean: v = std::exp(mean) * reach; break;
                case Attr::AnisoRatio: v = std::exp(std::min(hi - lo, 60.0f)); break;
                default: {
                    // The effective rank of the covariance, as the erank
                    // regularizer has it (shaders/per_splat_losses.slang).
                    double e[3], sum = 0.0;
                    for (int k = 0; k < 3; k++) {
                        e[k] = std::exp(2.0 * (double)(s[k] + grow - hi));
                        sum += e[k];
                    }
                    double h = 0.0;
                    for (int k = 0; k < 3; k++) {
                        const double q = std::max(e[k] / sum, 1e-30);
                        h -= q * std::log(q);
                    }
                    v = (float)std::exp(h);
                }
            }
            out[(size_t)i] = v;
        }
        return true;
    }

    if (is_colour(a)) {
        std::vector<float> rgb;
        if (!doc.colours(rgb) || (int64_t)rgb.size() != n * 3) return false;
#pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < n; i++)
            out[(size_t)i] = colour_scalar(a, &rgb[(size_t)i * 3]);
        return true;
    }

    if (a == Attr::CameraDistance) {
        const std::vector<float> cams = doc.camera_centres();
        const int64_t nc = (int64_t)cams.size() / 3;
        if (nc == 0) return false;
        const float* p = doc.positions();
        const float unit = (float)(doc.placement().s / doc.view_frame().s);
#pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < n; i++) {
            float best = std::numeric_limits<float>::max();
            for (int64_t c = 0; c < nc; c++) {
                const float dx = p[i*3] - cams[(size_t)c*3];
                const float dy = p[i*3+1] - cams[(size_t)c*3+1];
                const float dz = p[i*3+2] - cams[(size_t)c*3+2];
                best = std::min(best, dx*dx + dy*dy + dz*dz);
            }
            out[(size_t)i] = std::sqrt(best) * unit;
        }
        return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

void AttrHistogram::build(const std::vector<float>& v, const uint8_t* alive,
                          const uint8_t* sel, bool log_axis, double fixed_lo,
                          double fixed_hi) {
    log = log_axis;
    all.assign(kBins, 0);
    selected.assign(kBins, 0);
    peak = 0;
    live = 0;
    const int64_t n = (int64_t)v.size();
    auto axis = [&](float x) -> double {
        if (!log) return (double)x;
        return x > 0.0f ? std::log10((double)x) : -std::numeric_limits<double>::infinity();
    };

    // The range, from a sample: a percentile does not get better for being
    // taken over more of the same distribution.
    const int64_t step = std::max<int64_t>(1, n / 200000);
    std::vector<double> sample;
    for (int64_t i = 0; i < n; i += step) {
        if (alive && !alive[i]) continue;
        const double a = axis(v[(size_t)i]);
        if (std::isfinite(a)) sample.push_back(a);
    }
    if (sample.empty()) { lo = 0.0; hi = 1.0; return; }
    auto pct = [&](double q) {
        const size_t k = (size_t)std::clamp(q * (double)(sample.size() - 1), 0.0,
                                            (double)(sample.size() - 1));
        std::nth_element(sample.begin(), sample.begin() + (ptrdiff_t)k, sample.end());
        return sample[k];
    };
    lo = pct(0.002);
    hi = pct(0.998);
    if (!(hi > lo)) {
        const double pad = std::max(std::fabs(lo) * 1e-3, 1e-6);
        lo -= pad;
        hi += pad;
    }
    const double pad = (hi - lo) * 0.02;
    lo -= pad;
    hi += pad;
    if (fixed_hi > fixed_lo) {
        lo = fixed_lo;
        hi = fixed_hi;
    }

    const double k = kBins / (hi - lo);
    for (int64_t i = 0; i < n; i++) {
        if (alive && !alive[i]) continue;
        const double a = axis(v[(size_t)i]);
        if (std::isnan(a)) continue;
        const int b = (int)std::clamp((a - lo) * k, 0.0, (double)(kBins - 1));
        all[(size_t)b]++;
        if (sel && sel[i]) selected[(size_t)b]++;
        live++;
    }
    for (uint32_t c : all) peak = std::max(peak, c);
}

double AttrHistogram::value_at(double frac) const {
    const double a = lo + (hi - lo) * frac;
    return log ? std::pow(10.0, a) : a;
}

double AttrHistogram::frac_of(double value) const {
    const double a = log ? (value > 0 ? std::log10(value) : lo) : value;
    return (a - lo) / (hi - lo);
}

void select_by_range(const std::vector<float>& v, const AttrHistogram& h,
                     double f0, double f1, bool outside, const uint8_t* alive,
                     std::vector<uint8_t>& out) {
    const int64_t n = (int64_t)v.size();
    out.assign((size_t)n, 0);
    if (f0 > f1) std::swap(f0, f1);
    const double inf = std::numeric_limits<double>::infinity();
    // An end dragged to the edge of the plot means "and everything past it".
    const double a0 = f0 <= 0.0 ? -inf : h.lo + (h.hi - h.lo) * f0;
    const double a1 = f1 >= 1.0 ? inf : h.lo + (h.hi - h.lo) * f1;
    const bool log = h.log;
#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < n; i++) {
        if (alive && !alive[i]) continue;
        const float x = v[(size_t)i];
        if (std::isnan(x)) continue;
        const double a = log ? (x > 0.0f ? std::log10((double)x) : -inf) : (double)x;
        if ((a >= a0 && a <= a1) != outside) out[(size_t)i] = 255;
    }
}

void select_by_colour(const std::vector<float>& rgb, const float* samples,
                      int k, float tolerance, float lightness_weight,
                      const uint8_t* alive, std::vector<uint8_t>& out) {
    const int64_t n = (int64_t)rgb.size() / 3;
    out.assign((size_t)n, 0);
    if (k <= 0) return;
    std::vector<float> lab((size_t)k * 3);
    for (int j = 0; j < k; j++) srgb_to_oklab(samples + j * 3, lightness_weight, &lab[(size_t)j * 3]);
    const float t2 = tolerance * tolerance;
#pragma omp parallel for schedule(static)
    for (int64_t i = 0; i < n; i++) {
        if (alive && !alive[i]) continue;
        float c[3];
        srgb_to_oklab(&rgb[(size_t)i * 3], lightness_weight, c);
        for (int j = 0; j < k; j++) {
            const float d0 = c[0] - lab[(size_t)j*3], d1 = c[1] - lab[(size_t)j*3+1],
                        d2 = c[2] - lab[(size_t)j*3+2];
            if (d0*d0 + d1*d1 + d2*d2 <= t2) {
                out[(size_t)i] = 255;
                break;
            }
        }
    }
}

}  // namespace gui
