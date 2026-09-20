// Which image pairs to match. Kept separate from the matcher so the pairing
// strategy and the descriptor-matching algorithm compose independently: any
// IFeatureMatcher can be driven over any pair list.
//
// Exhaustive (all i<j) and sequential (a sliding window over a video) are
// generated here; Prefilter (GPU pair selection by mini-matching top-scale
// subsets) needs the features and lives in sfm/feature/PairSelection.h.
//
// Sequential and Prefilter each miss what the other finds, so by default each
// takes the other's list too: `--loop-closure` and `--prefilter-sequential`,
// see matchFeatureDir.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sfm {

enum class PairMode { Exhaustive, Sequential, Prefilter };

// Each image with the next `overlap` of its sequence and, with `quadratic`,
// the ones 2^k ahead for k < overlap (COLMAP's quadratic_overlap). A window
// never crosses from one `run` (folder) into another; empty = one sequence.
inline std::vector<std::pair<uint32_t, uint32_t>> sequentialPairs(
    uint32_t n, int overlap, bool quadratic, const std::vector<uint32_t>& run = {}) {
    const uint32_t ov = overlap > 0 ? (uint32_t)overlap : 10u;
    std::map<uint32_t, std::vector<uint32_t>> seqs;
    for (uint32_t i = 0; i < n; i++) seqs[run.size() == n ? run[i] : 0].push_back(i);
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    for (const auto& kv : seqs) {
        const std::vector<uint32_t>& s = kv.second;
        for (size_t a = 0; a < s.size(); a++) {
            for (uint32_t k = 1; k <= ov && a + k < s.size(); k++)
                pairs.emplace_back(s[a], s[a + k]);
            for (uint32_t k = 0; quadratic && k < ov && k < 31; k++) {
                const size_t b = a + ((size_t)1 << k);
                if (b >= s.size()) break;
                pairs.emplace_back(s[a], s[b]);
            }
        }
    }
    for (auto& p : pairs)
        if (p.first > p.second) std::swap(p.first, p.second);
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

// The sequence id of each image name: its parent folder.
inline std::vector<uint32_t> folderRuns(const std::vector<std::string>& names) {
    std::map<std::string, uint32_t> ids;
    std::vector<uint32_t> run(names.size());
    for (size_t i = 0; i < names.size(); i++) {
        const size_t slash = names[i].find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : names[i].substr(0, slash);
        run[i] = ids.emplace(dir, (uint32_t)ids.size()).first->second;
    }
    return run;
}

// Every pair for Exhaustive; the plain window (no quadratic links, one
// sequence) for Sequential.
inline std::vector<std::pair<uint32_t, uint32_t>> generatePairs(uint32_t n, PairMode mode,
                                                                int overlap = 10) {
    if (mode != PairMode::Exhaustive) return sequentialPairs(n, overlap, false);
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    pairs.reserve((size_t)n * (n - 1) / 2);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = i + 1; j < n; j++) pairs.emplace_back(i, j);
    return pairs;
}

}  // namespace sfm
