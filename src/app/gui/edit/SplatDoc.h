#pragma once

// The Gaussian document: a splat PLY open for editing, drawn out of the
// engine scene slot it was uploaded into.
//
// A delete rides on the opacity array and a selection on the DC colour, so an
// edit costs ONE attribute upload rather than the whole model -- which is
// what makes a brush stroke over a million Gaussians feel like a brush
// stroke. The cull that makes it free is the projection's ALPHA_THRESHOLD.

#include "app/gui/edit/EditDoc.h"
#include "checkpoint/SplatPly.h"

#include <mutex>

namespace gui {

class SplatDoc : public EditDoc {
public:
    // `slot` is the engine scene slot already holding this model and `mu` the
    // mutex its renderer takes; both outlive the document. `to_view` is the
    // file frame into the one the viewport navigates, row-major 3x4.
    SplatDoc(spirula::SplatCloud cloud, const std::string& source,
             const float to_view[12], int slot, std::mutex* mu);

    Kind kind() const override { return Kind::Splats; }
    std::vector<SaveTarget> save_targets() const override;
    void save(int target, const std::string& path) override;
    std::string default_save_path(int target) const override;
    void revert_display() override;

protected:
    void publish_impl(bool geometry) override;

private:
    spirula::SplatCloud _c;
    std::vector<float> _opacity;      // upload scratch, alive-masked
    std::vector<float> _dc;           // upload scratch, selection-tinted
    int _slot = -1;
    std::mutex* _mu = nullptr;
};

}  // namespace gui
