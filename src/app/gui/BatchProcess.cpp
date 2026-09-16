// BatchProcess.cpp -- see BatchProcess.h.

#include "app/gui/BatchProcess.h"

#include "app/AppPaths.h"
#include "app/TrainerCore.h"
#include "app/gui/SourceList.h"
#include "app/gui/TrainPreset.h"
#include "data/Json.h"
#include "data/JsonWrite.h"
#include "i18n/catalog/Gui.h"
#include "i18n/catalog/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace gui {

namespace msg = spirula::i18n::msg::gui;

namespace {

BatchIssue issue_of(const spirula::i18n::Msg& m, BatchStage stage, bool fatal,
                    std::string arg = {}) {
    BatchIssue i;
    i.text = &m;
    i.arg = std::move(arg);
    i.fatal = fatal;
    i.stage = stage;
    return i;
}

std::string batch_list_path() {
    return (fs::path(app::config_dir()) / "batch.json").string();
}

// The nearest ancestor of `p` that exists. "" when none does, which on every
// platform this runs on means the drive is not there.
fs::path existing_ancestor(fs::path p) {
    std::error_code ec;
    for (; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p, ec)) return p;
        if (p.parent_path() == p) break;   // hit the root
    }
    return {};
}

// Parse one of the per-run overrides. Returns false only when the text is
// there and is not a whole number in [lo, hi] -- an empty field is "use the
// preset", which is a success with `out` left alone.
bool parse_override(const std::string& text, int lo, int hi, int* out) {
    size_t i = 0;
    while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
    if (i == text.size()) return true;   // empty or whitespace: still unset
    size_t end = 0;
    long long v = 0;
    try {
        v = std::stoll(text.substr(i), &end);
    } catch (const std::exception&) {
        return false;
    }
    // Trailing junk ("300px") is a typo, not a number.
    for (size_t j = i + end; j < text.size(); j++)
        if (!std::isspace((unsigned char)text[j])) return false;
    if (v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

bool is_set(const std::string& text) {
    return text.find_first_not_of(" \t") != std::string::npos;
}

// The three overrides, in one place so the check and the builder cannot
// disagree about what a legal value is.
struct OverrideSpec {
    const std::string& text;
    int lo, hi;
    const char* flag;
    int TrainConfig::* field;
};
std::vector<OverrideSpec> overrides_of(const BatchRow& row) {
    return {{row.cap_max_override, 1, 1000000000, "cap_max", &TrainConfig::cap_max},
            {row.sh_degree_override, 0, 4, "sh_degree", &TrainConfig::sh_degree},
            {row.iterations_override, 1, 1000000000, "num_iterations",
             &TrainConfig::num_iterations}};
}

// The training config a preset reference resolves to, plus the built-in name
// to record in the run's config.json.
bool config_of(const BatchPreset& p, TrainConfig& cfg,
               std::set<std::string>& touched, std::string& base,
               std::string& error) {
    cfg = TrainConfig();
    if (p.path.empty()) {
        base = p.name.empty() ? "3dgs" : p.name;
        if (!train_apply_preset(cfg, base)) {
            error = spirula::i18n::format(
                spirula::i18n::msg::log::err_unknown_preset, {base});
            return false;
        }
        return true;
    }
    try {
        TrainPreset tp = load_preset(p.path);
        cfg = tp.cfg;
        touched = tp.touched;
        base = tp.base;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

// Two rows do the same work when they train the same dataset with the same
// settings. The same dataset twice is ordinary -- comparing two presets on one
// capture is exactly what a batch is for.
bool same_train_work(const BatchRow& a, const BatchRow& b) {
    if (a.dataset != b.dataset || a.dataset.empty()) return false;
    if (a.cap_max_override != b.cap_max_override ||
        a.sh_degree_override != b.sh_degree_override ||
        a.iterations_override != b.iterations_override)
        return false;
    if (a.train_presets.size() != b.train_presets.size()) return false;
    for (size_t i = 0; i < a.train_presets.size(); i++) {
        if (a.train_presets[i].path != b.train_presets[i].path) return false;
        if (a.train_presets[i].path.empty() &&
            a.train_presets[i].name != b.train_presets[i].name)
            return false;
    }
    return true;
}

// ---- the JSON shape of one row -------------------------------------------

void write_preset(JsonWriter& w, const char* key, const BatchPreset& p) {
    w.key(key).object();
    w.field("path", p.path);
    w.field("name", p.name);
    w.end();
}

BatchPreset read_preset(const JsonValue* v) {
    BatchPreset p;
    if (!v || !v->is_object()) return p;
    if (const JsonValue* s = v->find("path")) p.path = s->as_string();
    if (const JsonValue* s = v->find("name")) p.name = s->as_string();
    return p;
}

}  // namespace


std::string batch_issue_line(const BatchIssue& issue) {
    if (!issue.text) return issue.raw;
    if (issue.arg.empty()) return issue.text->get();
    return spirula::i18n::format(*issue.text, {issue.arg});
}


bool batch_has_error(const std::vector<BatchIssue>& issues) {
    for (const BatchIssue& i : issues)
        if (i.fatal) return true;
    return false;
}


int batch_num_runs(const BatchRow& row) {
    return row.train_presets.empty() ? 1 : (int)row.train_presets.size();
}


std::vector<BatchTask> batch_plan(const std::vector<BatchRow>& rows) {
    std::vector<BatchTask> out;
    for (int i = 0; i < (int)rows.size(); i++) {
        const BatchRow& r = rows[i];
        if (!r.enabled) continue;
        if (r.does(BatchStage::Dataset))
            out.push_back({i, BatchStage::Dataset, 0});
        const int runs = r.does(BatchStage::Train) ? batch_num_runs(r) : 0;
        for (int k = 0; k < runs; k++)
            out.push_back({i, BatchStage::Train, k});
        if (r.does(BatchStage::Mesh))
            for (int k = 0; k < std::max(runs, 1); k++)
                out.push_back({i, BatchStage::Mesh, k});
    }
    return out;
}


std::string batch_dataset_workspace(const BatchRow& row) {
    if (!row.dataset.empty()) return row.dataset;
    if (row.sources.empty()) return {};
    std::vector<PrepInput> probe;
    for (const std::string& p : row.sources) probe.push_back(make_source(p, true));
    return default_workspace(probe);
}


// ---------------------------------------------------------------------------
// The pre-flight
// ---------------------------------------------------------------------------

namespace {

void check_dataset_stage(const BatchRow& row, const BatchCapabilities& caps,
                         std::vector<BatchIssue>& out) {
    constexpr BatchStage kSt = BatchStage::Dataset;
    std::error_code ec;

    if (row.sources.empty()) {
        out.push_back(issue_of(msg::chk_sources_empty, kSt, true));
    } else {
        for (const std::string& p : row.sources) {
            if (p.empty() || !fs::exists(p, ec)) {
                out.push_back(issue_of(msg::chk_source_missing, kSt, true, p));
            } else if (fs::is_directory(p, ec)) {
                if (!folder_has_images(p))
                    out.push_back(issue_of(msg::chk_source_no_images, kSt, true, p));
            } else if (!is_video_path(p)) {
                out.push_back(issue_of(msg::chk_source_unsupported, kSt, true, p));
            }
        }
    }

    DatasetSettings s;
    if (!row.dataset_preset.path.empty()) {
        if (!fs::is_regular_file(row.dataset_preset.path, ec)) {
            out.push_back(issue_of(msg::chk_preset_missing, kSt, true,
                                   row.dataset_preset.path));
            return;
        }
        try {
            s = load_dataset_preset(row.dataset_preset.path).s;
        } catch (const std::exception& e) {
            out.push_back(issue_of(msg::chk_preset_unreadable, kSt, true,
                                   row.dataset_preset.path));
            BatchIssue raw;
            raw.raw = e.what();
            raw.fatal = true;
            raw.stage = kSt;
            out.push_back(raw);
            return;
        }
    }

    const bool have_engine = s.colmap_engine ? caps.colmap : caps.builtin_sfm;
    if (!have_engine) out.push_back(issue_of(msg::chk_engine_unavailable, kSt, true));

    if (s.sfm.prep.mask_enable) {
        // A batch has no clicks -- they belong to the frames they were drawn
        // on and a preset cannot carry them -- so the text prompt is the only
        // prompt there is.
        if (s.mask.prompt.empty())
            out.push_back(issue_of(msg::chk_mask_no_prompt, kSt, true));
        if (s.sfm.prep.force_external_masking) {
            // mask.py resolves its own model by name; nothing here can say
            // whether it is there.
        } else if (!caps.masking) {
            out.push_back(issue_of(msg::chk_masking_unavailable, kSt, true));
        } else if (caps.mask_model_ready && !caps.mask_model_ready(s.mask_model_id)) {
            out.push_back(issue_of(msg::chk_mask_model_missing, kSt, true,
                                   s.mask_model_id));
        }
    }

    if (s.sfm.geometry.enable) {
        if (!caps.geometry)
            out.push_back(issue_of(msg::chk_geometry_unavailable, kSt, true));
        else if (caps.geometry_model_ready &&
                 !caps.geometry_model_ready(s.sfm.geometry.model))
            out.push_back(issue_of(msg::chk_geometry_model_missing, kSt, true,
                                   s.sfm.geometry.model));
    }

    // A preset made for photographs, pointed at a video (or the other way
    // round): it still runs, and it picks the wrong pairing strategy.
    if (!row.sources.empty() && !row.dataset_preset.path.empty()) {
        bool any_video = false, any_photos = false;
        for (const std::string& p : row.sources) {
            const bool vid = !fs::is_directory(p, ec) && is_video_path(p);
            any_video = any_video || vid;
            any_photos = any_photos || !vid;
        }
        if (s.sfm.data_type == 1 && !any_video && any_photos)
            out.push_back(issue_of(msg::chk_capture_is_photos, kSt, false));
        if (s.sfm.data_type != 1 && any_video && !any_photos)
            out.push_back(issue_of(msg::chk_capture_is_video, kSt, false));
    }

    const std::string ws = batch_dataset_workspace(row);
    if (ws.empty()) {
        out.push_back(issue_of(msg::chk_workspace_empty, kSt, true));
    } else if (fs::exists(ws, ec) && !fs::is_directory(ws, ec)) {
        out.push_back(issue_of(msg::chk_output_is_file, kSt, true, ws));
    } else if (existing_ancestor(fs::absolute(ws, ec)).empty()) {
        out.push_back(issue_of(msg::chk_output_unusable, kSt, true, ws));
    } else if (folder_looks_like_dataset(ws)) {
        // A run left to itself REUSES a model rather than spending an hour
        // rebuilding one, which is right for adding masks and wrong for
        // someone who expected a fresh reconstruction.
        out.push_back(issue_of(msg::chk_dataset_has_model, kSt, false, ws));
    }
}

void check_train_stage(const BatchRow& row, const BatchCapabilities& caps,
                       bool made_here, std::vector<BatchIssue>& out) {
    constexpr BatchStage kSt = BatchStage::Train;
    std::error_code ec;

    if (!made_here) {
        if (row.dataset.empty())
            out.push_back(issue_of(msg::chk_dataset_empty, kSt, true));
        else if (!fs::exists(row.dataset, ec))
            out.push_back(issue_of(msg::chk_dataset_missing, kSt, true, row.dataset));
        else if (!fs::is_directory(row.dataset, ec))
            out.push_back(issue_of(msg::chk_dataset_not_a_dir, kSt, true, row.dataset));
        else if (!folder_looks_like_dataset(row.dataset))
            out.push_back(issue_of(msg::chk_dataset_unreadable, kSt, true, row.dataset));
    }

    int scratch = 0;
    const spirula::i18n::Msg* bad[] = {&msg::chk_bad_max_splats,
                                       &msg::chk_bad_sh_degree,
                                       &msg::chk_bad_steps};
    const std::vector<OverrideSpec> specs = overrides_of(row);
    for (size_t i = 0; i < specs.size(); i++)
        if (!parse_override(specs[i].text, specs[i].lo, specs[i].hi, &scratch))
            out.push_back(issue_of(*bad[i], kSt, true, specs[i].text));

    const std::vector<BatchPreset> one{BatchPreset{}};
    const std::vector<BatchPreset>& presets =
        row.train_presets.empty() ? one : row.train_presets;
    for (const BatchPreset& p : presets) {
        TrainConfig cfg;
        std::set<std::string> touched;
        std::string base, error;
        if (!p.path.empty() && !fs::is_regular_file(p.path, ec)) {
            out.push_back(issue_of(msg::chk_preset_missing, kSt, true, p.path));
            continue;
        }
        if (!config_of(p, cfg, touched, base, error)) {
            out.push_back(issue_of(p.path.empty() ? msg::chk_preset_unknown
                                                  : msg::chk_preset_unreadable,
                                   kSt, true, p.path.empty() ? p.name : p.path));
            continue;
        }
        // Everything the trainer refuses outright, asked before the queue has
        // spent an hour getting to this row.
        if (std::string what = spirula::train_config_unsupported(cfg); !what.empty())
            out.push_back(issue_of(msg::chk_unsupported, kSt, true, what));
        // A missing image folder is a warning, not an error: the COLMAP and
        // Nerfstudio parsers both index images by what the reconstruction
        // names, which can be somewhere else entirely.
        if (!made_here && !row.dataset.empty() && !cfg.image_dir.empty() &&
            fs::is_directory(row.dataset, ec) &&
            !fs::exists(fs::path(row.dataset) / cfg.image_dir, ec))
            out.push_back(issue_of(msg::chk_images_missing, kSt, false, cfg.image_dir));
    }

    const std::string dir = row.output_dir.empty() && !row.dataset.empty()
                                ? (fs::path(row.dataset) / "outputs").string()
                                : row.output_dir;
    if (!dir.empty()) {
        if (fs::exists(dir, ec) && !fs::is_directory(dir, ec))
            out.push_back(issue_of(msg::chk_output_is_file, kSt, true, dir));
        else if (!made_here && existing_ancestor(fs::absolute(dir, ec)).empty())
            out.push_back(issue_of(msg::chk_output_unusable, kSt, true, dir));
    }

    if (!caps.device) out.push_back(issue_of(msg::chk_no_device, kSt, true));
}

void check_mesh_stage(const BatchRow& row, std::vector<BatchIssue>& out) {
    constexpr BatchStage kSt = BatchStage::Mesh;
    std::error_code ec;

    if (row.model.empty() && !row.does(BatchStage::Train)) {
        out.push_back(issue_of(msg::chk_mesh_no_model, kSt, true));
    } else if (!row.model.empty() && !fs::exists(row.model, ec)) {
        out.push_back(issue_of(msg::chk_mesh_model_missing, kSt, true, row.model));
    }

    MeshJob job;
    if (!row.mesh_preset.path.empty()) {
        if (!fs::is_regular_file(row.mesh_preset.path, ec)) {
            out.push_back(issue_of(msg::chk_preset_missing, kSt, true,
                                   row.mesh_preset.path));
            return;
        }
        try {
            job = load_mesh_preset(row.mesh_preset.path).job;
        } catch (const std::exception& e) {
            out.push_back(issue_of(msg::chk_preset_unreadable, kSt, true,
                                   row.mesh_preset.path));
            BatchIssue raw;
            raw.raw = e.what();
            raw.fatal = true;
            raw.stage = kSt;
            out.push_back(raw);
            return;
        }
    }
    // Meshing without cameras is a much rougher mesh, and a row that neither
    // names a dataset nor builds one gets exactly that.
    if (job.use_data && row.dataset.empty() && !row.does(BatchStage::Dataset) &&
        !row.does(BatchStage::Train))
        out.push_back(issue_of(msg::chk_mesh_no_dataset, kSt, false));
}

}  // namespace


std::vector<BatchIssue> batch_check_row(const BatchRow& row,
                                        const std::vector<BatchRow>& all,
                                        int index,
                                        const BatchCapabilities& caps) {
    std::vector<BatchIssue> out;
    if (!row.enabled) return out;

    const bool anything = row.does(BatchStage::Dataset) ||
                          row.does(BatchStage::Train) || row.does(BatchStage::Mesh);
    if (!anything) {
        out.push_back(issue_of(msg::chk_nothing_to_do, BatchStage::Dataset, false));
        return out;
    }

    if (row.does(BatchStage::Dataset)) check_dataset_stage(row, caps, out);
    if (row.does(BatchStage::Train))
        check_train_stage(row, caps, row.does(BatchStage::Dataset), out);
    if (row.does(BatchStage::Mesh)) check_mesh_stage(row, out);

    // ---- what the LIST says, rather than the row ----
    const std::string ws = row.does(BatchStage::Dataset)
                               ? batch_dataset_workspace(row) : std::string();
    for (int i = 0; i < (int)all.size(); i++) {
        if (i == index || !all[i].enabled) continue;
        const BatchRow& other = all[i];
        // Two rows building a dataset into one folder would fight over it.
        if (!ws.empty() && other.does(BatchStage::Dataset) &&
            batch_dataset_workspace(other) == ws) {
            out.push_back(issue_of(msg::chk_dataset_collision,
                                   BatchStage::Dataset, true, ws));
            break;
        }
    }
    for (int i = 0; i < (int)all.size(); i++) {
        if (i == index || !all[i].enabled) continue;
        if (row.does(BatchStage::Train) && !row.does(BatchStage::Dataset) &&
            same_train_work(row, all[i]) && all[i].does(BatchStage::Train)) {
            out.push_back(issue_of(msg::chk_dataset_duplicate, BatchStage::Train,
                                   false, row.dataset));
            break;
        }
    }
    // A row training a dataset a LATER row builds runs before it exists.
    if (row.does(BatchStage::Train) && !row.does(BatchStage::Dataset) &&
        !row.dataset.empty()) {
        for (int i = index + 1; i < (int)all.size(); i++) {
            if (!all[i].enabled || !all[i].does(BatchStage::Dataset)) continue;
            if (batch_dataset_workspace(all[i]) != row.dataset) continue;
            out.push_back(issue_of(msg::chk_dataset_made_later, BatchStage::Train,
                                   false, row.dataset));
            break;
        }
    }
    return out;
}


// ---------------------------------------------------------------------------
// What each stage runs with
// ---------------------------------------------------------------------------

bool batch_build_dataset_job(const BatchRow& row, const std::string& ffmpeg_exe,
                             std::vector<PrepInput>& sources,
                             DatasetSettings& settings, std::string& workspace,
                             std::string& error) {
    sources.clear();
    settings = DatasetSettings{};
    if (!row.dataset_preset.path.empty()) {
        try {
            settings = load_dataset_preset(row.dataset_preset.path).s;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    for (const std::string& p : row.sources)
        sources.push_back(make_source(p, settings.use_found_masks));
    probe_sources(sources, ffmpeg_exe);

    if (row.dataset_preset.path.empty()) {
        apply_capture_defaults(sources, settings.sfm, settings.colmap);
    } else {
        // A preset decides everything the capture does not, and what the
        // capture does decide is the sphere to warp and the lens of its
        // views: a rectilinear model on a fisheye clip reconstructs nothing.
        DatasetSettings capture;
        apply_capture_defaults(sources, capture.sfm, capture.colmap);
        app::Pano360Options& pano = settings.sfm.prep.pano;
        if (!any_pano360(sources)) {
            pano.mode = app::Pano360Mode::Off;
        } else {
            if (pano.mode == app::Pano360Mode::Off)
                pano.mode = capture.sfm.prep.pano.mode;
            if (pano.size <= 0) reset_pano_size(sources, pano);
            settings.sfm.camera_mode = capture.sfm.camera_mode;
            settings.colmap.camera_mode = capture.colmap.camera_mode;
        }
    }
    resolve_source_lenses(sources, settings.sfm, settings.colmap);
    assign_source_subdirs(sources);
    refresh_subcameras(sources);
    normalize_source_lenses(sources, settings.sfm.camera_model);

    workspace = batch_dataset_workspace(row);
    if (workspace.empty()) {
        error = "no output folder for this row";
        return false;
    }
    return true;
}


bool batch_build_train_config(const BatchRow& row, int variant,
                              const std::string& dataset,
                              const std::string& image_dir,
                              const std::string& mask_dir, bool mask_flipped,
                              TrainConfig& cfg, std::string& preset_base,
                              std::string& error) {
    BatchPreset p;
    if (variant >= 0 && variant < (int)row.train_presets.size())
        p = row.train_presets[(size_t)variant];

    std::set<std::string> touched;
    if (!config_of(p, cfg, touched, preset_base, error)) return false;

    cfg.data = dataset;
    if (!image_dir.empty()) cfg.image_dir = image_dir;
    if (!mask_dir.empty()) cfg.mask_dir = mask_dir;
    if (!mask_dir.empty()) cfg.flip_mask = mask_flipped;
    cfg.output_dir_prefix = row.output_dir.empty()
                                ? (fs::path(dataset) / "outputs").string()
                                : row.output_dir;
    // Every run gets its own timestamped subfolder, so a batch re-run never
    // writes over what the last one produced and two rows may share a folder.
    cfg.output_dir_name.clear();
    // Unattended: nothing will open a browser, and binding a port per run is
    // one more thing that can fail between two datasets. The native viewport
    // does not go through the web viewer, so a run is still watchable.
    cfg.disable_viewer = true;

    // The row's own overrides go on top of the preset, and count as set by
    // hand: --quality moves cap_max and num_iterations, and a number typed
    // into the row is not something a macro gets to overwrite.
    for (const OverrideSpec& o : overrides_of(row)) {
        int v = 0;
        if (!parse_override(o.text, o.lo, o.hi, &v)) {
            error = spirula::i18n::format(
                spirula::i18n::msg::log::err_bad_flag_value, {o.flag, o.text});
            return false;
        }
        if (!is_set(o.text)) continue;
        cfg.*o.field = v;
        touched.insert(o.flag);
    }

    // The same resolution order the trainer screen uses, so a run trains with
    // the values the options editor would have shown for this preset.
    train_resolve_macros(cfg, touched);
    return true;
}


bool batch_build_mesh_job(const BatchRow& row, const std::string& model,
                          const std::string& dataset, MeshJob& job,
                          std::string& error) {
    job = MeshJob{};
    if (!row.mesh_preset.path.empty()) {
        try {
            job = load_mesh_preset(row.mesh_preset.path).job;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    if (model.empty()) {
        error = "no model to mesh";
        return false;
    }
    job.checkpoint = model;
    job.output = default_mesh_output(model);
    // A row that names or builds a dataset says so; otherwise the child reads
    // it out of the run's own config.json, which is what a run folder carries.
    job.data_dir = job.use_data ? dataset : std::string();
    sanitize_mesh_job(job);
    return true;
}


// ---------------------------------------------------------------------------
// The list on disk
// ---------------------------------------------------------------------------

std::vector<BatchRow> load_batch_list() {
    std::vector<BatchRow> out;
    try {
        const JsonValue root = json_parse_file(batch_list_path());
        if (const JsonValue* rows = root.find("rows"); rows && rows->is_array()) {
            for (const JsonValue& j : rows->arr) {
                BatchRow r;
                if (const JsonValue* v = j.find("sources"); v && v->is_array())
                    for (const JsonValue& e : v->arr)
                        if (!e.as_string().empty()) r.sources.push_back(e.as_string());
                if (const JsonValue* v = j.find("dataset")) r.dataset = v->as_string();
                if (const JsonValue* v = j.find("model")) r.model = v->as_string();
                if (const JsonValue* v = j.find("output_dir"))
                    r.output_dir = v->as_string();
                if (const JsonValue* v = j.find("cap_max"))
                    r.cap_max_override = v->as_string();
                if (const JsonValue* v = j.find("sh_degree"))
                    r.sh_degree_override = v->as_string();
                if (const JsonValue* v = j.find("num_iterations"))
                    r.iterations_override = v->as_string();
                r.dataset_preset = read_preset(j.find("dataset_preset"));
                r.mesh_preset = read_preset(j.find("mesh_preset"));
                if (const JsonValue* v = j.find("train_presets"); v && v->is_array())
                    for (const JsonValue& e : v->arr)
                        r.train_presets.push_back(read_preset(&e));
                if (const JsonValue* v = j.find("stages"); v && v->is_array())
                    for (int i = 0; i < kNumBatchStages && i < (int)v->arr.size(); i++)
                        r.stages[i] = v->arr[(size_t)i].as_bool();
                if (const JsonValue* v = j.find("enabled")) r.enabled = v->as_bool(true);
                out.push_back(std::move(r));
            }
            return out;
        }
        // A queue written before rows could do more than train.
        if (const JsonValue* jobs = root.find("jobs"); jobs && jobs->is_array()) {
            for (const JsonValue& j : jobs->arr) {
                BatchRow r;
                r.stages[(int)BatchStage::Train] = true;
                if (const JsonValue* v = j.find("dataset")) r.dataset = v->as_string();
                if (const JsonValue* v = j.find("output_dir"))
                    r.output_dir = v->as_string();
                if (const JsonValue* v = j.find("cap_max"))
                    r.cap_max_override = v->as_string();
                if (const JsonValue* v = j.find("sh_degree"))
                    r.sh_degree_override = v->as_string();
                if (const JsonValue* v = j.find("num_iterations"))
                    r.iterations_override = v->as_string();
                BatchPreset p;
                if (const JsonValue* v = j.find("preset_path")) p.path = v->as_string();
                if (const JsonValue* v = j.find("preset_name")) p.name = v->as_string();
                if (p.name.empty()) p.name = "3dgs";
                r.train_presets.push_back(p);
                out.push_back(std::move(r));
            }
        }
    } catch (const std::exception&) {
        // No list yet, or one a crash left half-written. Starting empty is the
        // only useful answer either way.
    }
    return out;
}


void save_batch_list(const std::vector<BatchRow>& rows) {
    JsonWriter w;
    w.object();
    w.key("rows").array();
    for (const BatchRow& r : rows) {
        w.object();
        w.key("sources").array();
        for (const std::string& s : r.sources) w.value(s);
        w.end();
        w.field("dataset", r.dataset);
        w.field("model", r.model);
        w.field("output_dir", r.output_dir);
        w.field("cap_max", r.cap_max_override);
        w.field("sh_degree", r.sh_degree_override);
        w.field("num_iterations", r.iterations_override);
        write_preset(w, "dataset_preset", r.dataset_preset);
        write_preset(w, "mesh_preset", r.mesh_preset);
        w.key("train_presets").array();
        for (const BatchPreset& p : r.train_presets) {
            w.object();
            w.field("path", p.path);
            w.field("name", p.name);
            w.end();
        }
        w.end();
        w.key("stages").array();
        for (int i = 0; i < kNumBatchStages; i++) w.value(r.stages[i]);
        w.end();
        w.field("enabled", r.enabled);
        w.end();
    }
    w.end();
    w.end();

    FILE* f = std::fopen(batch_list_path().c_str(), "wb");
    if (!f) return;
    const std::string text = w.str();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

}  // namespace gui
