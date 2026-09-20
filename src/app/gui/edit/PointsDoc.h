#pragma once

// The sparse-reconstruction document: the seed cloud a dataset trains from,
// open for cleaning.
//
// Only the points are editable -- images and tracks are not -- which is what
// makes saving a row filter over the file the points came out of rather than
// a re-export (data/SparseEdit.h). A cloud with no reconstruction behind it
// is the same document with one save target fewer.

#include "app/gui/edit/EditDoc.h"
#include "data/DatasetParser.h"
#include "data/SparseEdit.h"

#include <functional>
#include <string>

namespace gui {

class PointsDoc : public EditDoc {
public:
    // `dataset_dir` is "" for a loose PLY; `source` is the file or folder the
    // points were read from. `show` is called with the display cloud whenever
    // it changes, which is how the GL preview is rebuilt.
    PointsDoc(ParsedDataset ds, PostSplitCameras post,
              const std::string& source, const std::string& dataset_dir,
              std::function<void(const ParsedDataset&, const PostSplitCameras&)> show);

    Kind kind() const override { return Kind::Points; }
    const spirula::i18n::Msg& element_name() const override;
    std::vector<SaveTarget> save_targets() const override;
    void save(int target, const std::string& path) override;
    std::string default_save_path(int target) const override;
    void revert_display() override;

    spirula::SparseFormat format() const { return _fmt; }

protected:
    void publish_impl(bool geometry) override;

private:
    ParsedDataset _ds;
    PostSplitCameras _post;
    ParsedDataset _display;
    std::string _dataset_dir;
    spirula::SparseFormat _fmt = spirula::SparseFormat::None;
    std::function<void(const ParsedDataset&, const PostSplitCameras&)> _show;
};

}  // namespace gui
