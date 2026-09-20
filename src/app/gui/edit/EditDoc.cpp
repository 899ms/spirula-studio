// EditDoc.cpp -- see EditDoc.h.

#include "app/gui/edit/EditDoc.h"

#include "i18n/catalog/Edit.h"

#include <algorithm>
#include <cmath>

namespace msg = spirula::i18n::msg::edit;

namespace gui {

namespace {

// What the history may hold before the oldest entries are dropped: a session
// going for an hour must not be the reason the machine runs out of memory.
constexpr size_t kMaxHistoryBytes = 256u << 20;
constexpr int kMaxHistoryOps = 96;

}  // namespace


void EditDoc::init(int64_t n, std::vector<float> positions, std::string source) {
    _count = n;
    _pos = std::move(positions);
    _source = std::move(source);
    _alive.assign((size_t)n, 1);
    _alive_count = n;
    _sel.resize(n);

    // The median distance from the median point, as the viewer frames a model
    // by: a trained scene has floaters, so a bounding box puts every default
    // radius a kilometre out.
    float c[3] = {0, 0, 0};
    if (n > 0) {
        const int64_t step = std::max<int64_t>(1, n / (1 << 20));
        std::vector<float> tmp;
        tmp.reserve((size_t)(n / step + 1));
        for (int d = 0; d < 3; d++) {
            tmp.clear();
            for (int64_t i = 0; i < n; i += step) tmp.push_back(_pos[(size_t)i * 3 + d]);
            std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
            c[d] = tmp[tmp.size() / 2];
        }
        tmp.clear();
        for (int64_t i = 0; i < n; i += step) {
            const float dx = _pos[(size_t)i * 3 + 0] - c[0];
            const float dy = _pos[(size_t)i * 3 + 1] - c[1];
            const float dz = _pos[(size_t)i * 3 + 2] - c[2];
            tmp.push_back(dx * dx + dy * dy + dz * dz);
        }
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        _extent = 2.0f * std::sqrt(std::max(tmp[tmp.size() / 2], 1e-24f));
    }
    for (int d = 0; d < 3; d++) _middle[d] = c[d];
    _extent = std::max(_extent, 1e-6f);
    const double side = 2.0 * (double)_extent;
    _radius_hint = (float)(1.5 * side / std::cbrt((double)std::max<int64_t>(n, 1)));
    _radius_hint = std::clamp(_radius_hint, _extent * 1e-4f, _extent * 0.25f);
}

void EditDoc::set_alive(int64_t i, bool a) {
    uint8_t& v = _alive[(size_t)i];
    if ((v != 0) == a) return;
    v = a ? 1 : 0;
    _alive_count += a ? 1 : -1;
}

void EditDoc::set_selection(const std::vector<uint8_t>& w) {
    _sel.assign(w);
}

void EditDoc::run(std::unique_ptr<EditOp> op) {
    op->apply(*this);
    _ops.resize((size_t)_head);
    _bytes = 0;
    for (auto& o : _ops) _bytes += o->bytes();
    _bytes += op->bytes();
    _ops.push_back(std::move(op));
    _head = (int)_ops.size();
    while ((int)_ops.size() > kMaxHistoryOps ||
           (_bytes > kMaxHistoryBytes && _ops.size() > 1)) {
        _bytes -= _ops.front()->bytes();
        _ops.erase(_ops.begin());
        _head--;
    }
    _edited = true;
}

void EditDoc::undo() {
    if (!can_undo()) return;
    _ops[(size_t)--_head]->undo(*this);
    _edited = true;
}

void EditDoc::redo() {
    if (!can_redo()) return;
    _ops[(size_t)_head++]->apply(*this);
    _edited = true;
}

const spirula::i18n::Msg* EditDoc::undo_name() const {
    return can_undo() ? &_ops[(size_t)_head - 1]->name() : nullptr;
}

const spirula::i18n::Msg* EditDoc::redo_name() const {
    return can_redo() ? &_ops[(size_t)_head]->name() : nullptr;
}

bool EditDoc::publish() {
    if (!_geom_dirty && !_display_dirty) return false;
    const bool geom = _geom_dirty;
    _geom_dirty = false;
    _display_dirty = false;
    publish_impl(geom);
    return true;
}


// ---------------------------------------------------------------------------
// Ops
// ---------------------------------------------------------------------------

namespace {

// Deletes are soft: the element is flagged, not removed, and compaction
// happens at save. Undo is then the same index list back the other way, which
// is why nothing here carries a copy of the data.
class HideOp : public EditOp {
public:
    HideOp(std::vector<int32_t> idx, std::vector<uint8_t> was, bool invert)
        : _idx(std::move(idx)), _was(std::move(was)), _invert(invert) {}

    void apply(EditDoc& doc) override {
        std::vector<uint8_t> w = doc.sel().weights();
        for (size_t k = 0; k < _idx.size(); k++) {
            doc.set_alive(_idx[k], false);
            w[(size_t)_idx[k]] = 0;
        }
        // What was deleted is no longer there to be selected, and a count
        // that goes on naming it is the first thing anyone queries.
        if (!_invert) doc.set_selection(w);
        doc.mark_geometry_dirty();
    }
    void undo(EditDoc& doc) override {
        std::vector<uint8_t> w = doc.sel().weights();
        for (size_t k = 0; k < _idx.size(); k++) {
            doc.set_alive(_idx[k], true);
            w[(size_t)_idx[k]] = _was[k];
        }
        if (!_invert) doc.set_selection(w);
        doc.mark_geometry_dirty();
    }
    const spirula::i18n::Msg& name() const override {
        return _invert ? msg::op_isolate : msg::op_delete;
    }
    size_t bytes() const override {
        return _idx.size() * (sizeof(int32_t) + 1) + 32;
    }

private:
    std::vector<int32_t> _idx;
    std::vector<uint8_t> _was;
    bool _invert;
};

class RevealOp : public EditOp {
public:
    explicit RevealOp(std::vector<int32_t> idx) : _idx(std::move(idx)) {}
    void apply(EditDoc& doc) override {
        for (int32_t i : _idx) doc.set_alive(i, true);
        doc.mark_geometry_dirty();
    }
    void undo(EditDoc& doc) override {
        for (int32_t i : _idx) doc.set_alive(i, false);
        doc.mark_geometry_dirty();
    }
    const spirula::i18n::Msg& name() const override { return msg::op_restore; }
    size_t bytes() const override { return _idx.size() * sizeof(int32_t) + 32; }

private:
    std::vector<int32_t> _idx;
};

class SelectOp : public EditOp {
public:
    SelectOp(std::vector<uint8_t> prev, std::vector<uint8_t> next,
             const spirula::i18n::Msg& name)
        : _prev(rle_encode(prev)), _next(rle_encode(next)),
          _n(prev.size()), _name(&name) {}

    void apply(EditDoc& doc) override { put(doc, _next); }
    void undo(EditDoc& doc) override { put(doc, _prev); }
    const spirula::i18n::Msg& name() const override { return *_name; }
    size_t bytes() const override { return _prev.size() + _next.size() + 48; }

private:
    void put(EditDoc& doc, const std::vector<uint8_t>& rle) {
        std::vector<uint8_t> w(_n, 0);
        rle_decode(rle, w);
        doc.set_selection(w);
        doc.mark_display_dirty();
    }
    std::vector<uint8_t> _prev, _next;
    size_t _n;
    const spirula::i18n::Msg* _name;
};

}  // namespace


std::unique_ptr<EditOp> make_hide_op(EditDoc& doc, bool invert) {
    std::vector<int32_t> idx;
    std::vector<uint8_t> was;
    const uint8_t* alive = doc.alive();
    const Selection& s = doc.sel();
    for (int64_t i = 0; i < doc.count(); i++) {
        if (!alive[i]) continue;
        if (s.selected(i) == invert) continue;
        idx.push_back((int32_t)i);
        was.push_back(s.weight(i));
    }
    return std::make_unique<HideOp>(std::move(idx), std::move(was), invert);
}

std::unique_ptr<EditOp> make_reveal_op(EditDoc& doc) {
    std::vector<int32_t> idx;
    const uint8_t* alive = doc.alive();
    for (int64_t i = 0; i < doc.count(); i++)
        if (!alive[i]) idx.push_back((int32_t)i);
    return std::make_unique<RevealOp>(std::move(idx));
}

std::unique_ptr<EditOp> make_select_op(EditDoc& doc, std::vector<uint8_t> next,
                                       const spirula::i18n::Msg& name) {
    return std::make_unique<SelectOp>(doc.sel().weights(), std::move(next), name);
}

}  // namespace gui
