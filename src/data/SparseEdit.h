#pragma once

// Writing an edited sparse reconstruction back where it came from.
//
// The write is a ROW FILTER over the files the reconstruction was read from,
// so everything an edit does not touch -- a COLMAP camera table, the other
// keys of a transforms.json -- survives untouched. A Metashape export is not
// ours to rewrite, so the edit lands beside it as a Nerfstudio dataset, which
// parse_dataset reads first from then on.

#include "core/Similarity.h"
#include "data/DatasetParser.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spirula {

enum class SparseFormat { None, Colmap, Nerfstudio, Metashape };

// What `dataset_dir` is, by the same probe order parse_dataset uses.
SparseFormat sparse_format_of(const std::string& dataset_dir);

// The dataset folder a user meant by `path`, or "" when it names no
// reconstruction. The dataset, its `sparse/`, its `sparse/0` and a
// `cameras.bin` inside it all have to arrive at the same folder.
std::string resolve_sparse_dir(const std::string& path);

// What an edit kept. Points are indexed as ParsedDataset::points holds them,
// which is the file's own order; images are NAMED rather than indexed,
// because the parser sorts its camera list by filename and the file does not.
struct SparseKeep {
    std::vector<uint8_t> points;            // 0 = drop
    std::vector<std::string> drop_images;   // file names, any path prefix
};

// The files as one editing session first found them. `keep` indexes THEIR
// rows and `moved` starts from THEIR poses, so a second save has to filter
// these again rather than the files the first save left behind.
struct SparseBaseline {
    SparseFormat format = SparseFormat::None;
    bool text = false;               // COLMAP: the .txt spelling
    std::string model_dir;           // COLMAP
    std::string images, points, frames;
    std::string meta;                // Nerfstudio / Metashape: the JSON text
    std::string ply_rel;
    ColmapPoints3D cloud;
    bool loaded() const { return format != SparseFormat::None; }
};

// Rewrite the reconstruction, copying each replaced file to `<name>.orig`
// once. `moved` is in ParsedDataset's RAW frame (COLMAP's world; a
// transforms.json with applied_transform undone). `base` null re-reads disk.
std::vector<std::string> sparse_write_filtered(const std::string& dataset_dir,
                                               const SparseKeep& keep,
                                               const Sim3* moved = nullptr,
                                               SparseBaseline* base = nullptr);

// A plain point cloud, for anything that has no reconstruction behind it.
void write_ply_points(const std::string& path, const double* xyz,
                      const uint8_t* rgb, int64_t n, const uint8_t* keep,
                      const Sim3* moved = nullptr);

}  // namespace spirula
