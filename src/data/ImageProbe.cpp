#include "data/ImageProbe.h"

#include "core/ExrImage.h"
#include "external/stb_image.h"

#include <cctype>
#include <filesystem>

bool probe_image_size(const char* path, int* w, int* h) {
    if (!path || !*path) return false;
    if (exr::is_exr(path)) {
        exr::Info info;
        if (!exr::probe(path, info).empty()) return false;
        *w = info.width;
        *h = info.height;
        return true;
    }
    int ch;
    return stbi_info(path, w, h, &ch) != 0;
}

namespace {

bool header_has_alpha(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".exr") return false;
    int w = 0, h = 0, ch = 0;
    return stbi_info(path.c_str(), &w, &h, &ch) && (ch == 2 || ch == 4);
}

bool any_transparent(const std::string& path) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, 0);
    if (!px) return false;
    bool found = false;
    if (ch == 2 || ch == 4)
        for (size_t i = 0, n = (size_t)w * h; i < n && !found; i++)
            found = px[i * ch + ch - 1] < 255;
    stbi_image_free(px);
    return found;
}

}  // namespace

std::vector<uint8_t> probe_alpha_masks(const std::vector<std::string>& paths) {
    std::vector<uint8_t> flags(paths.size(), 0);
    std::vector<size_t> with_alpha;
    for (size_t i = 0; i < paths.size(); i++)
        if (header_has_alpha(paths[i])) {
            flags[i] = 1;
            with_alpha.push_back(i);
        }
    if (with_alpha.empty()) return {};
    // First, middle and last: a capture exported one way is exported that way
    // throughout, and a full decode of each costs as much as loading it.
    const size_t n = with_alpha.size();
    for (size_t k : {(size_t)0, n / 2, n - 1})
        if (any_transparent(paths[with_alpha[k]])) return flags;
    return {};
}
