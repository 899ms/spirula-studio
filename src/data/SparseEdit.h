#pragma once

// Writing an edited sparse reconstruction back where it came from.
//
// Only the point cloud is edited, so the write is a ROW FILTER over the file
// that produced it: a COLMAP track, a Nerfstudio frame list, everything a
// reader cares about survives untouched. A Metashape export is not ours to
// rewrite, so the edit lands beside it as a Nerfstudio dataset -- which
// parse_dataset reads first from then on.

#include <cstdint>
#include <string>
#include <vector>

namespace spirula {

enum class SparseFormat { None, Colmap, Nerfstudio, Metashape };

// What `dataset_dir` is, by the same probe order parse_dataset uses.
SparseFormat sparse_format_of(const std::string& dataset_dir);

// Rewrite the reconstruction under `dataset_dir`, keeping the points whose
// flag is set -- indexed as ParsedDataset::points holds them. Each replaced
// file is copied to `<name>.orig` first, once. Returns what was written.
std::vector<std::string> sparse_write_filtered(const std::string& dataset_dir,
                                               const std::vector<uint8_t>& keep);

// A plain point cloud, for anything that has no reconstruction behind it.
void write_ply_points(const std::string& path, const double* xyz,
                      const uint8_t* rgb, int64_t n, const uint8_t* keep);

}  // namespace spirula
