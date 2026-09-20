// SparseEdit.cpp -- see SparseEdit.h.

#include "data/SparseEdit.h"

#include "data/DatasetParser.h"
#include "data/Json.h"
#include "data/JsonWrite.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

// Images are matched by the leaf of their path: the reconstruction stores a
// name relative to its own image folder and the parser stores an absolute
// one, and the leaf is the part the two always agree on.
std::string leaf_of(const std::string& path) {
    return fs::path(path).filename().string();
}

std::set<std::string> leaf_set(const std::vector<std::string>& v) {
    std::set<std::string> s;
    for (const std::string& p : v) s.insert(leaf_of(p));
    return s;
}

template <typename T>
T read_le(const char*& p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

template <typename T>
void put_le(std::string& out, T v) {
    out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

// images.bin: a count, then per image an id, a pose, a camera id, a
// null-terminated name and the 2D observations. Rows are copied byte for
// byte, so nothing an edit did not ask about is rewritten.
std::string filter_images_bin(const std::string& src,
                              const std::set<std::string>& drop,
                              std::set<int32_t>& dropped_ids) {
    const char* p = src.data();
    const char* end = src.data() + src.size();
    if ((size_t)(end - p) < sizeof(uint64_t))
        throw std::runtime_error("images.bin is truncated");
    const uint64_t n = read_le<uint64_t>(p);
    std::string out;
    out.reserve(src.size());
    out.resize(sizeof(uint64_t));
    uint64_t kept = 0;
    for (uint64_t i = 0; i < n; i++) {
        const char* row = p;
        if (end - p < 4 + 8 * 7 + 4) throw std::runtime_error("images.bin is truncated");
        const int32_t id = read_le<int32_t>(p);
        p += 8 * 7 + 4;                              // qvec, tvec, camera_id
        const char* name = p;
        while (p < end && *p) p++;
        if (p >= end) throw std::runtime_error("images.bin is truncated");
        const std::string image_name(name, (size_t)(p - name));
        p++;                                          // the terminator
        if ((size_t)(end - p) < sizeof(uint64_t))
            throw std::runtime_error("images.bin is truncated");
        const uint64_t pts = read_le<uint64_t>(p);
        const size_t pt_bytes = (size_t)pts * (8 + 8 + 8);
        if ((size_t)(end - p) < pt_bytes) throw std::runtime_error("images.bin is truncated");
        p += pt_bytes;
        if (drop.count(leaf_of(image_name))) {
            dropped_ids.insert(id);
            continue;
        }
        out.append(row, (size_t)(p - row));
        kept++;
    }
    std::memcpy(&out[0], &kept, sizeof(uint64_t));
    return out;
}

// points3D.bin: the same, except that a track entry naming a dropped image
// has to go with it, which makes the row a rewrite rather than a copy.
std::string filter_points3d_bin(const std::string& src,
                                const std::vector<uint8_t>& keep,
                                const std::set<int32_t>& dropped_ids) {
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
        const char* head = p;
        if (end - p < 8 + 24 + 3 + 8 + 8)
            throw std::runtime_error("points3D.bin is truncated");
        p += 8 + 24 + 3 + 8;                          // id, xyz, rgb, error
        const size_t head_bytes = (size_t)(p - head);
        const uint64_t track = read_le<uint64_t>(p);
        const char* track_at = p;
        const size_t track_bytes = (size_t)track * 2 * sizeof(int32_t);
        if ((size_t)(end - p) < track_bytes)
            throw std::runtime_error("points3D.bin is truncated");
        p += track_bytes;
        if (i < keep.size() && !keep[(size_t)i]) continue;
        out.append(head, head_bytes);
        if (dropped_ids.empty()) {
            put_le<uint64_t>(out, track);
            out.append(track_at, track_bytes);
        } else {
            std::string live;
            uint64_t n_live = 0;
            for (uint64_t t = 0; t < track; t++) {
                const char* e = track_at + (size_t)t * 2 * sizeof(int32_t);
                int32_t image_id;
                std::memcpy(&image_id, e, sizeof(int32_t));
                if (dropped_ids.count(image_id)) continue;
                live.append(e, 2 * sizeof(int32_t));
                n_live++;
            }
            put_le<uint64_t>(out, n_live);
            out += live;
        }
        kept++;
    }
    std::memcpy(&out[0], &kept, sizeof(uint64_t));
    return out;
}

// The text model, one record per non-comment line -- except images.txt, where
// a record is two lines and the second may be empty.
std::string filter_images_txt(const std::string& src,
                              const std::set<std::string>& drop,
                              std::set<int32_t>& dropped_ids) {
    std::string out;
    out.reserve(src.size());
    size_t pos = 0;
    auto take_line = [&](size_t& at, std::string& line) {
        if (at >= src.size()) return false;
        size_t eol = src.find('\n', at);
        if (eol == std::string::npos) eol = src.size();
        line = src.substr(at, std::min(eol, src.size()) - at);
        at = eol + 1;
        return true;
    };
    std::string line;
    while (pos < src.size()) {
        const size_t start = pos;
        if (!take_line(pos, line)) break;
        size_t b = 0;
        while (b < line.size() && std::isspace((unsigned char)line[b])) b++;
        if (b >= line.size() || line[b] == '#') {
            out.append(src, start, pos - start);
            continue;
        }
        // id qw qx qy qz tx ty tz camera_id name
        const size_t obs_start = pos;
        std::string obs;
        take_line(pos, obs);
        int32_t id = 0;
        char name[1024] = {0};
        double d[7];
        int cam = 0;
        if (std::sscanf(line.c_str() + b, "%d %lf %lf %lf %lf %lf %lf %lf %d %1023s",
                        &id, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6],
                        &cam, name) == 10 &&
            drop.count(leaf_of(name))) {
            dropped_ids.insert(id);
            continue;
        }
        (void)obs_start;
        out.append(src, start, pos - start);
    }
    return out;
}

std::string filter_points3d_txt(const std::string& src,
                                const std::vector<uint8_t>& keep,
                                const std::set<int32_t>& dropped_ids) {
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
        if (!record) {
            out.append(src, pos, std::min(next, src.size()) - pos);
            pos = next;
            continue;
        }
        const bool take = index >= keep.size() || keep[index] != 0;
        index++;
        if (!take) {
            pos = next;
            continue;
        }
        if (dropped_ids.empty()) {
            out.append(src, pos, std::min(next, src.size()) - pos);
            pos = next;
            continue;
        }
        // Rewrite the track: the first eight fields are the point, the rest
        // is (image_id, point2D_idx) pairs.
        std::string line = src.substr(b, t - b);
        const char* s = line.c_str();
        char* q = nullptr;
        std::string head;
        for (int f = 0; f < 8; f++) {
            const double v = std::strtod(s, &q);
            if (q == s) break;
            (void)v;
            head.append(s, (size_t)(q - s));
            s = q;
        }
        std::string track;
        while (true) {
            const long a = std::strtol(s, &q, 10);
            if (q == s) break;
            const char* mid = q;
            const long bi = std::strtol(mid, &q, 10);
            if (q == mid) break;
            if (!dropped_ids.count((int32_t)a)) {
                track += " " + std::to_string(a) + " " + std::to_string(bi);
            }
            s = q;
        }
        out += head + track + "\n";
        pos = next;
    }
    return out;
}

std::string nerf_ply_rel(const JsonValue& meta) {
    if (const JsonValue* v = meta.find("ply_file_path")) return v->as_string();
    return {};
}

ColmapPoints3D read_points_of(const std::string& dataset_dir,
                              const std::string& ply_rel) {
    if (ply_rel.empty()) return {};
    return read_ply_points((fs::path(dataset_dir) / ply_rel).string());
}

// The frames a Nerfstudio meta keeps, by the leaf of their file_path.
bool drop_frames(JsonValue& meta, const std::set<std::string>& drop) {
    if (drop.empty()) return false;
    JsonValue* frames = nullptr;
    for (auto& [k, v] : meta.obj)
        if (k == "frames") frames = &v;
    if (!frames || !frames->is_array()) return false;
    std::vector<JsonValue> kept;
    kept.reserve(frames->arr.size());
    for (JsonValue& f : frames->arr) {
        const JsonValue* fp = f.find("file_path");
        if (fp && drop.count(leaf_of(fp->as_string()))) continue;
        kept.push_back(std::move(f));
    }
    const bool changed = kept.size() != frames->arr.size();
    frames->arr = std::move(kept);
    return changed;
}

void set_string(JsonValue& obj, const char* key, const std::string& value) {
    for (auto& [k, v] : obj.obj)
        if (k == key) {
            v.type = JsonValue::Type::String;
            v.str = value;
            return;
        }
    JsonValue v;
    v.type = JsonValue::Type::String;
    v.str = value;
    obj.obj.emplace_back(key, std::move(v));
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


std::string resolve_sparse_dir(const std::string& path) {
    std::error_code ec;
    fs::path p(path);
    if (p.empty()) return {};
    if (fs::is_regular_file(p, ec)) p = p.parent_path();
    if (!fs::is_directory(p, ec)) return {};

    // A folder of models is not a model: `sparse/` holds `0`, `1`, ...
    if (sparse_format_of(p.string()) == SparseFormat::None) {
        std::vector<fs::path> subs;
        for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec),
             end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) subs.push_back(it->path());
        std::sort(subs.begin(), subs.end());
        for (const fs::path& s : subs)
            if (sparse_format_of(s.string()) != SparseFormat::None) {
                p = s;
                break;
            }
    }
    if (sparse_format_of(p.string()) == SparseFormat::None) return {};

    // ... and a model folder is not the dataset: strip the conventional tail
    // so the images beside it are found too.
    auto conventional = [](const std::string& leaf) {
        if (leaf == "sparse" || leaf == "colmap") return true;
        if (leaf.empty()) return false;
        for (char c : leaf)
            if (!std::isdigit((unsigned char)c)) return false;
        return true;
    };
    fs::path q = p;
    for (int i = 0; i < 3; i++) {
        if (!conventional(q.filename().string())) break;
        q = q.parent_path();
        if (q.empty()) break;
        if (sparse_format_of(q.string()) != SparseFormat::None) p = q;
    }
    return p.string();
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
                                               const SparseKeep& keep) {
    std::vector<std::string> written;
    const std::set<std::string> drop = leaf_set(keep.drop_images);
    switch (sparse_format_of(dataset_dir)) {
        case SparseFormat::Colmap: {
            bool text = false;
            const std::string model = find_colmap_model(dataset_dir, "", &text);
            if (model.empty())
                throw std::runtime_error("no COLMAP points3D under " + dataset_dir);
            std::set<int32_t> dropped_ids;
            if (!drop.empty()) {
                const fs::path ip = fs::path(model) /
                                    (text ? "images.txt" : "images.bin");
                const std::string src = read_file(ip);
                const std::string body =
                    text ? filter_images_txt(src, drop, dropped_ids)
                         : filter_images_bin(src, drop, dropped_ids);
                keep_original(ip);
                write_file(ip, body);
                written.push_back(ip.string());
            }
            const fs::path pp = fs::path(model) /
                                (text ? "points3D.txt" : "points3D.bin");
            const std::string src = read_file(pp);
            keep_original(pp);
            write_file(pp, text ? filter_points3d_txt(src, keep.points, dropped_ids)
                                : filter_points3d_bin(src, keep.points, dropped_ids));
            written.push_back(pp.string());
            break;
        }
        case SparseFormat::Nerfstudio: {
            const fs::path meta_path = fs::path(dataset_dir) / "transforms.json";
            JsonValue meta = json_parse(read_file(meta_path));
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
                             pts.num(), keep.points.data());
            written.push_back(ply.string());
            const bool frames_changed = drop_frames(meta, drop);
            if (frames_changed || !meta.has("ply_file_path")) {
                set_string(meta, "ply_file_path", rel);
                keep_original(meta_path);
                JsonWriter w;
                json_write(w, meta);
                write_file(meta_path, w.str());
                written.push_back(meta_path.string());
            }
            break;
        }
        case SparseFormat::Metashape: {
            DatasetParserConfig cfg;
            JsonValue meta = metashape_meta(dataset_dir, cfg);
            const ColmapPoints3D pts =
                read_points_of(dataset_dir, nerf_ply_rel(meta));
            const fs::path ply = fs::path(dataset_dir) / "points3D_edited.ply";
            write_ply_points(ply.string(), pts.xyz.data(),
                             pts.rgb.empty() ? nullptr : pts.rgb.data(),
                             pts.num(), keep.points.data());
            written.push_back(ply.string());
            drop_frames(meta, drop);
            set_string(meta, "ply_file_path", "points3D_edited.ply");
            const fs::path meta_path = fs::path(dataset_dir) / "transforms.json";
            JsonWriter w;
            json_write(w, meta);
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
