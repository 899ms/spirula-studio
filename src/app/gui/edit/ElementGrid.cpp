// ElementGrid.cpp -- see ElementGrid.h.

#include "app/gui/edit/ElementGrid.h"

#include "app/gui/edit/EditDoc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace gui {

namespace {

// 21 bits per axis, biased to unsigned: a cell index further out than this is
// an outlier that gets clamped, and clamping only ever merges cells that were
// already empty of anything the distance test would accept.
constexpr int32_t kCoordBias = 1 << 20;
constexpr int32_t kCoordMax = (1 << 21) - 1;

uint64_t pack(const int32_t c[3]) {
    uint64_t k = 0;
    for (int d = 0; d < 3; d++) {
        const int32_t v = std::clamp(c[d] + kCoordBias, 0, kCoordMax);
        k = (k << 21) | (uint64_t)v;
    }
    return k;
}

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Lock-free union-find: path halving on reads and one compare-exchange per
// link, always pointing the larger index at the smaller so the order two
// threads reach a pair in cannot matter.
class UnionFind {
public:
    explicit UnionFind(int64_t n)
        : _p(std::make_unique<std::atomic<int32_t>[]>((size_t)n)) {
        for (int64_t i = 0; i < n; i++)
            _p[(size_t)i].store((int32_t)i, std::memory_order_relaxed);
    }
    int32_t find(int32_t x) const {
        while (true) {
            int32_t p = _p[(size_t)x].load(std::memory_order_relaxed);
            if (p == x) return x;
            const int32_t g = _p[(size_t)p].load(std::memory_order_relaxed);
            _p[(size_t)x].compare_exchange_weak(p, g, std::memory_order_relaxed);
            x = g;
        }
    }
    void unite(int32_t a, int32_t b) {
        while (true) {
            a = find(a);
            b = find(b);
            if (a == b) return;
            if (a > b) std::swap(a, b);
            int32_t expect = b;
            if (_p[(size_t)b].compare_exchange_weak(expect, a,
                                                    std::memory_order_relaxed))
                return;
        }
    }

private:
    std::unique_ptr<std::atomic<int32_t>[]> _p;
};

// Roots to dense labels, in first-seen order.
void label_roots(const UnionFind& uf, const uint8_t* alive, int64_t n,
                 std::vector<int32_t>& label, std::vector<int64_t>& sizes) {
    std::vector<int32_t> remap((size_t)n, -1);
    for (int64_t i = 0; i < n; i++) {
        if (alive && !alive[i]) continue;
        const int32_t root = uf.find((int32_t)i);
        int32_t& l = remap[(size_t)root];
        if (l < 0) {
            l = (int32_t)sizes.size();
            sizes.push_back(0);
        }
        label[(size_t)i] = l;
        sizes[(size_t)l]++;
    }
}

uint64_t table_size_for(int64_t n) {
    uint64_t m = 16;
    while (m < (uint64_t)std::max<int64_t>(n, 1) * 2) m <<= 1;
    return m;
}

}  // namespace


void ElementGrid::clear() {
    _pos = nullptr;
    _n = 0;
    _cell = 0.0f;
    _items.clear();
    _beg.clear();
    _hkey.clear();
    _hval.clear();
    _hmask = 0;
}

void ElementGrid::coords_of(const float* p, int32_t c[3]) const {
    for (int d = 0; d < 3; d++)
        c[d] = (int32_t)std::floor((p[d] - _origin[d]) / _cell);
}

int32_t ElementGrid::find_cell(const int32_t c[3]) const {
    if (_hval.empty()) return -1;
    const uint64_t key = pack(c);
    uint64_t slot = mix(key) & _hmask;
    while (true) {
        const int32_t v = _hval[(size_t)slot];
        if (v < 0) return -1;
        if (_hkey[(size_t)slot] == key) return v;
        slot = (slot + 1) & _hmask;
    }
}

void ElementGrid::build(const EditDoc& doc, float cell) {
    const float* pos = doc.positions();
    const int64_t n = doc.count();
    clear();
    if (n <= 0 || !(cell > 0.0f)) return;
    _pos = pos;
    _n = n;
    _cell = cell;
    const float* m = doc.middle();
    for (int d = 0; d < 3; d++) _origin[d] = m[d] - doc.extent();

    const uint64_t size = table_size_for(n);
    _hmask = size - 1;
    _hkey.assign((size_t)size, 0);
    _hval.assign((size_t)size, -1);

    // One pass: intern each element's cell and count it, in first-seen order.
    std::vector<int32_t> count;
    std::vector<int32_t> of_element((size_t)n);
    count.reserve((size_t)n / 4 + 16);
    for (int64_t i = 0; i < n; i++) {
        int32_t c[3];
        coords_of(pos + i * 3, c);
        const uint64_t key = pack(c);
        uint64_t slot = mix(key) & _hmask;
        while (_hval[(size_t)slot] >= 0 && _hkey[(size_t)slot] != key)
            slot = (slot + 1) & _hmask;
        if (_hval[(size_t)slot] < 0) {
            _hkey[(size_t)slot] = key;
            _hval[(size_t)slot] = (int32_t)count.size();
            count.push_back(0);
        }
        const int32_t k = _hval[(size_t)slot];
        of_element[(size_t)i] = k;
        count[(size_t)k]++;
    }

    _beg.assign(count.size() + 1, 0);
    for (size_t k = 0; k < count.size(); k++) _beg[k + 1] = _beg[k] + count[k];
    _items.resize((size_t)n);
    std::vector<int32_t> cursor(_beg.begin(), _beg.end() - 1);
    for (int64_t i = 0; i < n; i++)
        _items[(size_t)cursor[(size_t)of_element[(size_t)i]]++] = (int32_t)i;
}

// Converges on a cell holding about four elements. Each pass is one O(n)
// intern, so a handful of them costs tens of milliseconds at a million.
float ElementGrid::measure_spacing(const EditDoc& doc) {
    constexpr double kTarget = 4.0;
    const int64_t n = doc.count();
    if (n <= 0) return doc.suggested_radius();
    // Start from a SURFACE estimate rather than the volume one: that is where
    // a scanned scene's elements are, and starting ten times too coarse costs
    // several passes.
    float c = (float)(2.0 * doc.extent() / std::sqrt((double)n));
    c = std::max(c, doc.extent() * 1e-6f);
    for (int pass = 0; pass < 8; pass++) {
        build(doc, c);
        const int64_t used = occupied();
        if (used <= 0) break;
        const double occ = (double)n / (double)used;
        if (occ >= 2.0 && occ <= 8.0) break;
        // Halving the cell divides the occupancy by about four on a surface
        // and eight in a volume, so the exponent sits between the two.
        const double k = std::clamp(std::pow(kTarget / occ, 1.0 / 2.5), 0.25, 4.0);
        const float next = (float)(c * k);
        if (!(next > 0.0f) || next == c) break;
        c = next;
    }
    return _cell;
}


// ---------------------------------------------------------------------------
// Neighbour walks
// ---------------------------------------------------------------------------

void ElementGrid::grow(std::vector<uint8_t>& sel, float radius,
                       const uint8_t* alive, int64_t n) const {
    if (!built() || n != _n) return;
    const float r2 = radius * radius;
    const int reach = std::max(1, (int)std::ceil(radius / _cell));
    std::vector<uint8_t> next = sel;
#pragma omp parallel for schedule(dynamic, 4096)
    for (int64_t i = 0; i < _n; i++) {
        if (sel[(size_t)i] || (alive && !alive[i])) continue;
        const float* p = _pos + i * 3;
        int32_t c[3];
        coords_of(p, c);
        uint8_t best = 0;
        for (int32_t dz = -reach; dz <= reach && !best; dz++)
        for (int32_t dy = -reach; dy <= reach && !best; dy++)
        for (int32_t dx = -reach; dx <= reach && !best; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t k = find_cell(cc);
            if (k < 0) continue;
            for (int32_t t = _beg[(size_t)k]; t < _beg[(size_t)k + 1]; t++) {
                const int32_t j = _items[(size_t)t];
                if (!sel[(size_t)j]) continue;
                const float* q = _pos + (int64_t)j * 3;
                const float ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
                if (ex * ex + ey * ey + ez * ez <= r2) {
                    best = sel[(size_t)j];
                    break;
                }
            }
        }
        if (best) next[(size_t)i] = best;
    }
    sel.swap(next);
}

void ElementGrid::shrink(std::vector<uint8_t>& sel, float radius,
                         const uint8_t* alive, int64_t n) const {
    if (!built() || n != _n) return;
    const float r2 = radius * radius;
    const int reach = std::max(1, (int)std::ceil(radius / _cell));
    std::vector<uint8_t> next = sel;
#pragma omp parallel for schedule(dynamic, 4096)
    for (int64_t i = 0; i < _n; i++) {
        if (!sel[(size_t)i]) continue;
        const float* p = _pos + i * 3;
        int32_t c[3];
        coords_of(p, c);
        bool edge = false;
        for (int32_t dz = -reach; dz <= reach && !edge; dz++)
        for (int32_t dy = -reach; dy <= reach && !edge; dy++)
        for (int32_t dx = -reach; dx <= reach && !edge; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t k = find_cell(cc);
            if (k < 0) continue;
            for (int32_t t = _beg[(size_t)k]; t < _beg[(size_t)k + 1]; t++) {
                const int32_t j = _items[(size_t)t];
                if (sel[(size_t)j] || (alive && !alive[j])) continue;
                const float* q = _pos + (int64_t)j * 3;
                const float ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
                if (ex * ex + ey * ey + ez * ez <= r2) {
                    edge = true;
                    break;
                }
            }
        }
        if (edge) next[(size_t)i] = 0;
    }
    sel.swap(next);
}


void ElementGrid::components(float radius, const uint8_t* alive, int64_t n,
                             std::vector<int32_t>& label,
                             std::vector<int64_t>& sizes, const float* radii,
                             float scale) const {
    label.assign((size_t)n, -1);
    sizes.clear();
    if (!built() || n != _n) return;

    const float r2 = radius * radius;
    const int base_reach = std::max(1, (int)std::ceil(radius / _cell));
    UnionFind uf(_n);

    // With no per-element radius the pair test is symmetric, so half the
    // neighbourhood covers every pair once. With one it is not: the large
    // element has to reach the small one, and only its own walk can.
    const int32_t back = radii ? -1 : 0;
#pragma omp parallel for schedule(dynamic, 4096)
    for (int64_t i = 0; i < _n; i++) {
        if (alive && !alive[i]) continue;
        const float* p = _pos + i * 3;
        const float ri = radii ? radii[i] : 0.0f;
        const int reach = radii
            ? std::min(std::max(base_reach,
                                (int)std::ceil(2.0f * scale * ri / _cell)), 8)
            : base_reach;
        int32_t c[3];
        coords_of(p, c);
        for (int32_t dz = back * reach; dz <= reach; dz++)
        for (int32_t dy = (!radii && dz == 0 ? 0 : -reach); dy <= reach; dy++)
        for (int32_t dx = (!radii && dz == 0 && dy == 0 ? 0 : -reach);
             dx <= reach; dx++) {
            const int32_t cc[3] = {c[0] + dx, c[1] + dy, c[2] + dz};
            const int32_t k = find_cell(cc);
            if (k < 0) continue;
            const bool same = dx == 0 && dy == 0 && dz == 0;
            for (int32_t t = _beg[(size_t)k]; t < _beg[(size_t)k + 1]; t++) {
                const int32_t j = _items[(size_t)t];
                if (j == (int32_t)i || (!radii && same && j < (int32_t)i)) continue;
                if (alive && !alive[j]) continue;
                const float* q = _pos + (int64_t)j * 3;
                const float ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
                const float d2 = ex * ex + ey * ey + ez * ez;
                float lim2 = r2;
                if (radii) {
                    const float lim = scale * (ri + radii[j]);
                    lim2 = std::max(r2, lim * lim);
                }
                if (d2 <= lim2) uf.unite((int32_t)i, j);
            }
        }
    }

    label_roots(uf, alive, _n, label, sizes);
}


void components_from_pairs(const int32_t* pairs, int64_t n_pairs,
                           const uint8_t* alive, int64_t n,
                           std::vector<int32_t>& label,
                           std::vector<int64_t>& sizes) {
    label.assign((size_t)n, -1);
    sizes.clear();
    if (n <= 0) return;
    UnionFind uf(n);
    for (int64_t e = 0; e < n_pairs; e++) {
        const int32_t a = pairs[e * 2], b = pairs[e * 2 + 1];
        if (a < 0 || b < 0 || a >= n || b >= n) continue;
        if (alive && (!alive[a] || !alive[b])) continue;
        uf.unite(a, b);
    }
    label_roots(uf, alive, n, label, sizes);
}

}  // namespace gui
