// MeshPreset.cpp -- see MeshPreset.h.

#include "app/gui/MeshPreset.h"

#include "data/JsonField.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace gui {

namespace {

// X(key in the file, member of MeshJob). `checkpoint`, `data_dir` and
// `output` are deliberately absent: they are what the preset is applied TO.
#define SS_MESH_PRESET_FIELDS(X)                                              \
    X("use_data",          use_data)                                          \
    X("color",             color)                                             \
    X("max_cameras",       max_cameras)                                       \
    X("texture_size",      texture_size)                                      \
    X("iso",               iso)                                               \
    X("bisection_iters",   bisection_iters)                                   \
    X("merge_factor",      merge_factor)                                      \
    X("quality_iters",     quality_iters)                                     \
    X("floater_min_faces", floater_min_faces)                                 \
    X("cull_unseen",       cull_unseen)                                       \
    X("carve_k",           carve_k)                                           \
    X("extra_args",        extra_args)                                        \
    /* end */

}  // namespace


void sanitize_mesh_job(MeshJob& job) {
    job.color = std::clamp(job.color, 0, kNumMeshColorModes - 1);
    job.max_cameras = std::clamp(job.max_cameras, 0, 1000000);
    job.texture_size = std::clamp(job.texture_size, 0, 16384);
    job.bisection_iters = std::clamp(job.bisection_iters, 0, 32);
    job.quality_iters = std::clamp(job.quality_iters, 0, 32);
    job.floater_min_faces = std::clamp(job.floater_min_faces, 0, 10000000);
    job.carve_k = std::clamp(job.carve_k, 0, 64);
    job.merge_factor = std::clamp(job.merge_factor, 0.05f, 16.0f);

    // PLY cannot carry a texture and OBJ has no standard place for vertex
    // colors; the child refuses those combinations outright.
    if (job.color == 2) job.formats[0] = false;
    if (job.color == 1) job.formats[1] = false;
    bool any = false;
    for (int i = 0; i < kNumMeshFormats; i++) any = any || job.formats[i];
    if (!any) job.formats[job.color == 2 ? 3 : 0] = true;
}


void save_mesh_preset(const MeshPreset& p, const std::string& path) {
    PresetHeader head{p.name, p.description, path};
    JsonWriter w = preset_writer(PresetKind::Mesh, head);
    w.key("settings").object();
#define SS_MESH_EMIT(key, member) w.field_raw(key, json_field::emit(p.job.member));
    SS_MESH_PRESET_FIELDS(SS_MESH_EMIT)
#undef SS_MESH_EMIT
    w.key("formats").array();
    for (int i = 0; i < kNumMeshFormats; i++)
        if (p.job.formats[i]) w.value(kMeshFormats[i]);
    w.end();
    w.end();
    w.end();
    write_preset_file(path, w.str());
}


MeshPreset load_mesh_preset(const std::string& path) {
    PresetHeader head;
    const JsonValue root = read_preset_file(path, PresetKind::Mesh, head);
    const JsonValue* fields = root.find("settings");
    if (!fields || !fields->is_object())
        throw std::runtime_error(path + " holds no meshing settings");

    MeshPreset p;
    p.path = path;
    p.name = head.name;
    p.description = head.description;
#define SS_MESH_LOAD(key, member)                                             \
    if (const JsonValue* v = fields->find(key))                               \
        json_field::assign(p.job.member, *v);
    SS_MESH_PRESET_FIELDS(SS_MESH_LOAD)
#undef SS_MESH_LOAD

    if (const JsonValue* f = fields->find("formats"); f && f->is_array()) {
        for (int i = 0; i < kNumMeshFormats; i++) p.job.formats[i] = false;
        for (const JsonValue& e : f->arr)
            for (int i = 0; i < kNumMeshFormats; i++)
                if (e.as_string() == kMeshFormats[i]) p.job.formats[i] = true;
    }
    sanitize_mesh_job(p.job);
    if (p.name.empty()) p.name = std::filesystem::path(path).stem().string();
    return p;
}


void delete_mesh_preset(const std::string& path) {
    delete_preset_file(path, PresetKind::Mesh);
}


std::vector<MeshPreset> list_mesh_presets() {
    return list_preset_files(PresetKind::Mesh, load_mesh_preset);
}

}  // namespace gui
