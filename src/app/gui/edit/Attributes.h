#pragma once

// Per-element scalars a selection can be made from, and the histogram they
// are brushed on. One table: a name, where the numbers come from and how they
// want to be looked at, so the panel is generated from it and the twentieth
// attribute is a row rather than a feature.
//
// A document offers raw material -- a colour per element, the Gaussians
// themselves, its cameras -- and everything derived from that lives here.

#include "core/Similarity.h"

#include <cstdint>
#include <vector>

namespace spirula { namespace i18n { struct Msg; } }

namespace gui {

class EditDoc;

enum class Attr : int {
    // Where it is, in the coordinates it will be SAVED in.
    PosX = 0, PosY, PosZ,
    // A Gaussian's shape. "Extent" is the scale times sqrt(2 ln(opacity *
    // 255)): how far out it still reaches the rasterizer's alpha cut.
    Opacity, ScaleMax, ScaleMin, ScaleMean, ExtentMax, ExtentMin, ExtentMean,
    AnisoRatio, Erank,
    // Its base colour, display-referred and unclamped.
    Red, Green, Blue, Luma, ChromaU, ChromaV, Hue, Saturation,
    // A sparse point's distance to the nearest camera.
    CameraDistance,
    Count
};

// How the histogram bars are coloured, where that says something.
enum class AttrTint { None, Red, Green, Blue, Gray, Hue };

struct AttrInfo {
    Attr id;
    const spirula::i18n::Msg* name;
    const spirula::i18n::Msg* help;
    bool log;            // spans decades: bin its log10
    AttrTint tint;
    // A range that is the attribute's own rather than the data's: a hue is
    // 0..360 whatever the model holds. lo == hi leaves it to the data.
    double lo = 0.0, hi = 0.0;
};
const AttrInfo& attr_info(Attr a);

// What the document's CURRENT layer can be asked for, in panel order.
std::vector<Attr> attributes_of(const EditDoc& doc);
// One value per element; NaN where the element has none. False when the
// layer does not carry what `a` needs.
bool attribute_values(const EditDoc& doc, Attr a, std::vector<float>& out);

// 256 bins over a robust range of the LIVE values -- the 0.2th to the 99.8th
// percentile, so the three floaters a kilometre out do not squash everything
// else into one bin. The two end bins also hold what lies beyond them.
struct AttrHistogram {
    static constexpr int kBins = 256;
    bool log = false;
    double lo = 0.0, hi = 1.0;          // bin-axis range (log10 when `log`)
    std::vector<uint32_t> all, selected;
    uint32_t peak = 0;
    int64_t live = 0;

    void build(const std::vector<float>& v, const uint8_t* alive,
               const uint8_t* sel, bool log_axis, double fixed_lo = 0.0,
               double fixed_hi = 0.0);
    // The attribute value at a fraction 0..1 across the axis, and back.
    double value_at(double frac) const;
    double frac_of(double value) const;
};

// Live elements within the fractions [f0, f1] of `h`'s axis. An end at 0 or
// 1 is open: that is where the out-of-range values were binned. `outside` is
// the complement, which is also how a hue range runs through red.
void select_by_range(const std::vector<float>& v, const AttrHistogram& h,
                     double f0, double f1, bool outside, const uint8_t* alive,
                     std::vector<uint8_t>& out);

// Within `tolerance` of ANY of the `k` samples, in OKLab -- where equal
// distances look equally different. Both arrays are [., 3] display-referred;
// `lightness_weight` 0 ignores how bright a colour is.
void select_by_colour(const std::vector<float>& rgb, const float* samples,
                      int k, float tolerance, float lightness_weight,
                      const uint8_t* alive, std::vector<uint8_t>& out);

}  // namespace gui
