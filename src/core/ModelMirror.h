#pragma once

// The fallback host for every checkpoint the program downloads. Hugging Face
// and GitHub releases are unreachable from Mainland China; this ModelScope
// repository re-hosts the same files, flat, under their model-cache names.
// A file is only fetched from here after the upstream URL fails.

#include <string>

namespace spirula {

inline std::string model_mirror_url(const std::string& cache_name) {
    return "https://modelscope.cn/models/wbbaaoo/spirula/resolve/master/" + cache_name;
}

}  // namespace spirula
