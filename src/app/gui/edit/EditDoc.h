#pragma once

// A document open for editing: what was loaded, what has been done to it, and
// what is selected. Design: docs/notes/gui-editing-plan.md.
//
// An edit is an ENTRY IN A LIST, not a mutation of the loaded data. Replaying
// the list from the original is how undo works and how "save the edits, not
// the result" will work; a delete is soft (a flag, not a removal) and
// compaction happens at save.
//
// The three kinds differ only in what an element is: a Gaussian, a sparse
// point, a mesh vertex. Everything above this line is written once.

#include "app/gui/edit/Selection.h"
#include "i18n/Message.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gui {

class EditDoc;

// The only thing allowed to change a document.
struct EditOp {
    virtual ~EditOp() = default;
    virtual void apply(EditDoc& doc) = 0;
    virtual void undo(EditDoc& doc) = 0;
    // As it appears in the history list.
    virtual const spirula::i18n::Msg& name() const = 0;
    virtual size_t bytes() const = 0;
};

// What "Save a copy" can write this document as. `ext` is the extension a
// file target expects ("" for a target that writes a folder).
struct SaveTarget {
    const spirula::i18n::Msg* label = nullptr;
    std::string ext;
    bool folder = false;
};

class EditDoc {
public:
    enum class Kind { Splats, Points, Mesh };

    virtual ~EditDoc() = default;

    virtual Kind kind() const = 0;
    // What one element is called, for the counts on screen.
    virtual const spirula::i18n::Msg& element_name() const = 0;

    int64_t count() const { return _count; }
    int64_t alive_count() const { return _alive_count; }
    const uint8_t* alive() const { return _alive.data(); }
    Selection& sel() { return _sel; }
    const Selection& sel() const { return _sel; }

    // Element centres in the document's own coordinates, 3 floats each.
    const float* positions() const { return _pos.data(); }
    // Per-element world-space radius, or null when the kind has none (only a
    // Gaussian has an extent worth testing a stencil against).
    virtual const float* radii() const { return nullptr; }

    // The scene's extent and middle, by the median rather than the bounding
    // box: a trained model has floaters parked a kilometre out, and anything
    // sized off a box that holds them is sized wrong for everything else.
    float extent() const { return _extent; }
    const float* middle() const { return _middle; }
    // A neighbour distance that suits this cloud, from its density.
    float suggested_radius() const { return _radius_hint; }

    // ---- history ----
    // Runs `op` and puts it on the stack; drops the oldest entries when the
    // history is over its byte or count budget.
    void run(std::unique_ptr<EditOp> op);
    bool can_undo() const { return _head > 0; }
    bool can_redo() const { return _head < (int)_ops.size(); }
    void undo();
    void redo();
    const spirula::i18n::Msg* undo_name() const;
    const spirula::i18n::Msg* redo_name() const;
    size_t history_bytes() const { return _bytes; }
    int history_size() const { return (int)_ops.size(); }
    // Which entry each history row is, newest last; `head` is how many of
    // them are applied.
    const std::vector<std::unique_ptr<EditOp>>& history() const { return _ops; }
    int history_head() const { return _head; }

    // ---- what the ops write through ----
    void set_alive(int64_t i, bool a);
    void set_selection(const std::vector<uint8_t>& w);
    void mark_geometry_dirty() { _geom_dirty = true; _display_dirty = true; }
    void mark_display_dirty() { _display_dirty = true; }

    bool dirty() const { return _edited; }
    void mark_saved() { _edited = false; }

    // Push whatever changed to whatever is drawing this document, and say
    // whether it did. Once a frame, from the GUI thread.
    bool publish();
    // Put the renderer back the way it was found. A document's effect on what
    // is on screen ends with the document, whether its edits were saved or
    // thrown away -- the file is the only place either outcome is recorded.
    virtual void revert_display() = 0;

    // ---- saving ----
    virtual std::vector<SaveTarget> save_targets() const = 0;
    // Writes the live elements only. Throws std::runtime_error.
    virtual void save(int target, const std::string& path) = 0;
    // What "Save" (as opposed to "Save a copy") would overwrite, "" when the
    // document has no home to write back to.
    virtual std::string default_save_path(int target) const { (void)target; return {}; }

    const std::string& source_path() const { return _source; }

protected:
    // Fills the element table. Call from the concrete document's constructor
    // once its own arrays are in place.
    void init(int64_t n, std::vector<float> positions, std::string source);
    // The concrete document's half of publish(): `geometry` says the live set
    // changed, otherwise only the selection did.
    virtual void publish_impl(bool geometry) = 0;

    std::vector<uint8_t> _alive;

private:
    int64_t _count = 0;
    int64_t _alive_count = 0;
    std::vector<float> _pos;
    Selection _sel;
    float _extent = 1.0f;
    float _middle[3] = {0, 0, 0};
    float _radius_hint = 0.01f;
    std::string _source;

    std::vector<std::unique_ptr<EditOp>> _ops;
    int _head = 0;
    size_t _bytes = 0;
    bool _edited = false;
    bool _geom_dirty = true;
    bool _display_dirty = true;
};


// ---------------------------------------------------------------------------
// The ops phases 1 and 2 need
// ---------------------------------------------------------------------------

// Soft delete: the elements are flagged, not removed, and compaction happens
// at save. Undo is then the same list of indices back the other way.
std::unique_ptr<EditOp> make_hide_op(EditDoc& doc, bool invert);
// Every hidden element back.
std::unique_ptr<EditOp> make_reveal_op(EditDoc& doc);
// A selection change, so that a mis-aimed lasso is one Ctrl+Z away like
// everything else. Both sides are run-length encoded.
std::unique_ptr<EditOp> make_select_op(EditDoc& doc, std::vector<uint8_t> next,
                                       const spirula::i18n::Msg& name);

}  // namespace gui
