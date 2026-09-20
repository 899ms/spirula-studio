#pragma once

// The triangle-mesh document. An element is a VERTEX, as it is in every mesh
// editor, and deleting one takes the faces that used it with it -- which is
// what "cut this away" means to anyone who has done it before.
//
// A textured mesh is shown with its atlas sampled into vertex colours while
// it is being edited, because that is the only channel a selection has to
// show itself in. The file keeps its texture.

#include "app/gui/edit/EditDoc.h"
#include "mesh/MeshExport.h"

#include <functional>
#include <string>

namespace gui {

class MeshDoc : public EditDoc {
public:
    // `to_view` is the mesh's own frame into the one the viewport navigates,
    // row-major 3x4 (SplatViewer::mesh_to_normalized).
    MeshDoc(meshing::MeshData mesh, const std::string& source,
            const float to_view[12],
            std::function<void(const meshing::MeshData&, const float*)> show);

    Kind kind() const override { return Kind::Mesh; }
    const spirula::i18n::Msg& element_name() const override;
    std::vector<SaveTarget> save_targets() const override;
    void save(int target, const std::string& path) override;
    std::string default_save_path(int target) const override;
    void revert_display() override;

    int64_t live_faces() const { return _live_faces; }

protected:
    void publish_impl(bool geometry) override;

private:
    meshing::MeshData _m;
    meshing::MeshData _display;
    float _t2n[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    int64_t _live_faces = 0;
    std::function<void(const meshing::MeshData&, const float*)> _show;
};

}  // namespace gui
