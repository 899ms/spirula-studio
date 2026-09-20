#pragma once

// A uniform grid over element positions: the neighbour queries a selection
// grows by, and the union-find behind "keep only the big piece". Both are the
// same walk over the same cells, which is why they share a file.
//
// Hashed rather than dense. A dense table over a bounding box needs O(dim^3)
// cells to reach the spacing of a SURFACE, which is where splats and sparse
// points both sit -- a million of them want cells a thousandth of the scene
// across, and 10^9 mostly-empty cells is not a table anyone can afford.

#include <cstdint>
#include <vector>

namespace gui {

class EditDoc;

class ElementGrid {
public:
    // A cell that holds a handful of elements, which is also the one honest
    // measure of how far apart they are. A volume estimate from the count is
    // not: these sit on surfaces, and it comes out ten times too large.
    float measure_spacing(const EditDoc& doc);
    // Rebuild at an explicit cell size, which is the neighbour radius every
    // query below then works at.
    void build(const EditDoc& doc, float cell);
    void clear();
    bool built() const { return _cell > 0.0f; }
    float cell() const { return _cell; }

    // One shell of neighbours within `radius` added to (grow) or peeled off
    // (shrink) the selection. Dead elements never join it.
    void grow(std::vector<uint8_t>& sel, float radius, const uint8_t* alive,
              int64_t n) const;
    void shrink(std::vector<uint8_t>& sel, float radius, const uint8_t* alive,
                int64_t n) const;

    // Connected components over the live elements within `radius`; `label`
    // is -1 for a dead one. With `radii` they also link when they OVERLAP,
    // which is what joins one big sky Gaussian to its neighbours.
    void components(float radius, const uint8_t* alive, int64_t n,
                    std::vector<int32_t>& label, std::vector<int64_t>& sizes,
                    const float* radii = nullptr, float scale = 1.0f) const;

private:
    void coords_of(const float* p, int32_t c[3]) const;
    // Cell index, or -1 when nothing is in that cell.
    int32_t find_cell(const int32_t c[3]) const;
    // How many cells hold anything, for the spacing search.
    int64_t occupied() const { return (int64_t)_beg.size() - 1; }

    const float* _pos = nullptr;
    int64_t _n = 0;
    float _cell = 0.0f;
    float _origin[3] = {0, 0, 0};
    // Elements grouped by cell: `_beg[k] .. _beg[k+1]` index `_items`.
    std::vector<int32_t> _items;
    std::vector<int32_t> _beg;
    // Open addressing, power-of-two, linear probe: key -> cell index.
    std::vector<uint64_t> _hkey;
    std::vector<int32_t> _hval;
    uint64_t _hmask = 0;
};

// The same over a topology given outright -- a mesh's edges, two int32 per
// pair. Exact, and the only right answer where one exists: a distance rule
// calls a sparse floater several pieces and a dense wall one.
void components_from_pairs(const int32_t* pairs, int64_t n_pairs,
                           const uint8_t* alive, int64_t n,
                           std::vector<int32_t>& label,
                           std::vector<int64_t>& sizes);

}  // namespace gui
