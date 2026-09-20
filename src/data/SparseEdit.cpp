// SparseEdit.cpp -- see SparseEdit.h.

#include "data/SparseEdit.h"

#include "data/DatasetParser.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace spirula {

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(body.data(), (std::streamsize)body.size());
    f.flush();
    if (!f) throw std::runtime_error("write failed: " + p.string());
}

// Once, and only once: a second edit must not overwrite the copy of the file
// as it was before any of them.
void keep_original(const fs::path& p) {
    std::error_code ec;
    const fs::path bak = p.string() + ".orig";
    if (fs::exists(bak, ec) || !fs::exists(p, ec)) return;
    fs::copy_file(p, bak, ec);
}

template <typename T>
T read_le(const char*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

// points3D.bin is a count followed by variable-length records; keeping a row
// is copying its bytes, which is what leaves the tracks alone.
std::string filter_points3d_bin(const std::string& src,
                                const std::vector<uint8_t>& keep) {
    const char* p = src.data();
    const char* end = src.data() + src.size();
    if ((size_t)(end - p) < sizeof(uint64_t))
        throw std::runtime_error("points3D.bin is truncated");
    const uint64_t n = read_le<uint64_t>(p);
    std::string out;
    out.reserve(src.size());
    out.resize(sizeof(uint64_t));
    uint64_t kept = 0;
    for (uint64_t i = 0; i < n; i++) {
        const char* row = p;
        if (end - p < 8 + 24 + 3 + 8 + 8)
            throw std::runtime_error("points3D.bin is truncated");
        p += 8 + 24 + 3 + 8;
        const uint64_t track = read_le<uint64_t>(p);
        const size_t track_bytes = (size_t)track * 2 * sizeof(int32_t);
        if ((size_t)(end - p) < track_bytes)
            throw std::runtime_error("points3D.bin is truncated");
        p += track_bytes;
        if (i < keep.size() && !keep[(size_t)i]) continue;
        out.append(row, (size_t)(p - row));
        kept++;
    }
    std::memcpy(&out[0], &kept, sizeof(uint64_t));
    return out;
}

std::string filter_points3d_text(const std::string& src,
                                 const std::vector<uint8_t>& keep) {
    std::string out;
    out.reserve(src.size());
    size_t pos = 0, index = 0;
    while (pos < src.size()) {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        const size_t next = eol + 1;
        size_t b = pos, t = eol;
        while (b < t && (src[b] == ' ' || src[b] == '\t' || src[b] == '\r')) b++;
        const bool record = b < t && src[b] != '#';
        bool take = true;
        if (record) {
            take = index >= keep.size() || keep[index] != 0;
            index++;
        }
        if (take) out.append(src, pos, std::min(next, src.size()) - pos);
        pos = next;
    }
    return out;
}

// Where a Nerfstudio meta says its seed cloud is, relative to the dataset.
std::string nerf_ply_rel(const JsonValue& meta) {
    if (const JsonValue* v = meta.find("ply_file_path")) return v->as_string();
    return {};
}

ColmapPoints3D read_points_of(const std::string& dataset_dir,
                              const std::string& ply_rel) {
    if (ply_rel.empty()) return {};
    return read_ply_points((fs::path(dataset_dir) / ply_rel).string());
}

}  // namespace


SparseFormat sparse_format_of(const std::string& dataset_dir) {
    std::error_code ec;
    if (fs::exists(fs::path(dataset_dir) / "transforms.json", ec))
        return SparseFormat::Nerfstudio;
    if (!find_colmap_model(dataset_dir, "").empty()) return SparseFormat::Colmap;
    for (fs::directory_iterator it(fs::path(dataset_dir), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::string e = it->path().extension().string();
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        if (e == ".xml") return SparseFormat::Metashape;
    }
    return SparseFormat::None;
}


void write_ply_points(const std::string& path, const double* xyz,
                      const uint8_t* rgb, int64_t n, const uint8_t* keep) {
    int64_t kept = n;
    if (keep) {
        kept = 0;
        for (int64_t i = 0; i < n; i++) kept += keep[i] ? 1 : 0;
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f << "ply\nformat binary_little_endian 1.0\n";
    f << "element vertex " << kept << "\n";
    f << "property float x\nproperty float y\nproperty float z\n";
    f << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
    f << "end_header\n";
    for (int64_t i = 0; i < n; i++) {
        if (keep && !keep[i]) continue;
        const float p[3] = {(float)xyz[i * 3], (float)xyz[i * 3 + 1],
                            (float)xyz[i * 3 + 2]};
        f.write(reinterpret_cast<const char*>(p), sizeof p);
        const uint8_t c[3] = {rgb ? rgb[i * 3] : (uint8_t)200,
                              rgb ? rgb[i * 3 + 1] : (uint8_t)200,
                              rgb ? rgb[i * 3 + 2] : (uint8_t)200};
        f.write(reinterpret_cast<const char*>(c), sizeof c);
    }
    f.flush();
    if (!f) throw std::runtime_error("write failed: " + path);
}


std::vector<std::string> sparse_write_filtered(const std::string& dataset_dir,
                                               const std::vector<uint8_t>& keep) {
    std::vector<std::string> written;
    switch (sparse_format_of(dataset_dir)) {
        case SparseFormat::Colmap: {
            bool text = false;
            const std::string model = find_colmap_model(dataset_dir, "", &text);
            if (model.empty())
                throw std::runtime_error("no COLMAP points3D under " + dataset_dir);
            const fs::path p = fs::path(model) /
                               (text ? "points3D.txt" : "points3D.bin");
            const std::string src = read_file(p);
            keep_original(p);
            write_file(p, text ? filter_points3d_text(src, keep)
                               : filter_points3d_bin(src, keep));
            written.push_back(p.string());
            break;
        }
        case SparseFormat::Nerfstudio: {
            const fs::path meta_path = fs::path(dataset_dir) / "transforms.json";
            const JsonValue meta = json_parse(read_file(meta_path));
            std::string rel = nerf_ply_rel(meta);
            if (rel.empty()) rel = "points3D.ply";
            const fs::path ply = fs::path(dataset_dir) / rel;
            std::error_code ec;
            ColmapPoints3D pts;
            if (fs::exists(ply, ec)) {
                pts = read_ply_points(ply.string());
                keep_original(ply);
            }
            write_ply_points(ply.string(), pts.xyz.data(),
                             pts.rgb.empty() ? nullptr : pts.rgb.data(),
                             pts.num(), keep.data());
            written.push_back(ply.string());
            if (!meta.has("ply_file_path")) {
                JsonValue m = meta;
                JsonValue v;
                v.type = JsonValue::Type::String;
                v.str = rel;
                m.obj.emplace_back("ply_file_path", std::move(v));
                keep_original(meta_path);
                JsonWriter w;
                json_write(w, m);
                write_file(meta_path, w.str());
                written.push_back(meta_path.string());
            }
            break;
        }
        case SparseFormat::Metashape: {
            DatasetParserConfig cfg;
            const JsonValue meta = metashape_meta(dataset_dir, cfg);
            const std::string rel = nerf_ply_rel(meta);
            const ColmapPoints3D pts = read_points_of(dataset_dir, rel);
            const fs::path ply = fs::path(dataset_dir) / "points3D_edited.ply";
            write_ply_points(ply.string(), pts.xyz.data(),
                             pts.rgb.empty() ? nullptr : pts.rgb.data(),
                             pts.num(), keep.data());
            written.push_back(ply.string());
            JsonValue m = meta;
            bool set = false;
            for (auto& [k, v] : m.obj)
                if (k == "ply_file_path") {
                    v.type = JsonValue::Type::String;
                    v.str = "points3D_edited.ply";
                    set = true;
                }
            if (!set) {
                JsonValue v;
                v.type = JsonValue::Type::String;
                v.str = "points3D_edited.ply";
                m.obj.emplace_back("ply_file_path", std::move(v));
            }
            const fs::path meta_path = fs::path(dataset_dir) / "transforms.json";
            JsonWriter w;
            json_write(w, m);
            write_file(meta_path, w.str());
            written.push_back(meta_path.string());
            break;
        }
        default:
            throw std::runtime_error("no reconstruction to write back in " +
                                     dataset_dir);
    }
    return written;
}

}  // namespace spirula
