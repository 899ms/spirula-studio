#pragma once

// The sparse-reconstruction document: the seed cloud a dataset trains from
// and the cameras that produced it, open for cleaning.
//
// Two layers, because they are two different things to point a lasso at: the
// points, and the camera centres. Saving is a row filter over the files they
// were read from (data/SparseEdit.h) -- the tracks of a removed image go with
// it, and nothing else in the reconstruction is rewritten.

#include "app/gui/edit/EditDoc.h"
#include "data/DatasetParser.h"
#include "data/SparseEdit.h"

#include <functional>
#include <string>

namespace gui {

class PointsDoc : public EditDoc {
public:
    // `dataset_dir` is "" for a loose PLY. `show` is called with the display
    // cloud whenever it changes, which is how the GL preview is rebuilt; its
    // last argument is one flag per live camera, for the frustum highlight.
    using Show = std::function<void(const ParsedDataset&, const PostSplitCameras&,
                                    const uint8_t* selected)>;
    PointsDoc(ParsedDataset ds, PostSplitCameras post,
              const std::string& source, const std::string& dataset_dir,
              Show show);

    Kind kind() const override { return Kind::Points; }
    std::vector<SaveTarget> save_targets() const override;
    void save(int target, const std::string& path,
              std::atomic<int>* progress) override;
    std::string default_save_path(int target) const override;
    void revert_display() override;
    bool live_centers(dsparse::CenterTable& out) const override;

    spirula::SparseFormat format() const { return _fmt; }

protected:
    void publish_impl(bool geometry) override;

private:
    // The layer order, which is also what the panel offers.
    enum Layer { kPoints = 0, kCameras = 1 };
    // The display copy, with dead cameras dropped and its post-split table
    // rebuilt for them. Rebuilt only when the live camera set changes.
    void rebuild_display(bool cameras_changed);

    ParsedDataset _ds;
    PostSplitCameras _post;
    ParsedDataset _display;
    PostSplitCameras _post_display;
    std::vector<uint8_t> _cam_highlight;   // per live camera
    int64_t _live_cameras = -1;            // what the display was baked for
    std::string _dataset_dir;
    spirula::SparseFormat _fmt = spirula::SparseFormat::None;
    Show _show;
};

}  // namespace gui
