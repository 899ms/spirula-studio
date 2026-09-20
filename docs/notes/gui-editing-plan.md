# Editing in the GUI

A plan for the editing features users keep asking for: placing a model,
selecting parts of one and doing something to the selection, cleaning up a
model by attribute, fixing a mask by hand, laying out a camera move, and
splitting a reconstruction that came out as one model but is two.

Nothing here is a research problem. Every one of these features is a solved
interaction somewhere else — Blender, Photoshop, CloudCompare, MeshLab — and
the work is deciding what they share so that the eighth one is cheap rather
than an eighth of the code again.

## The mistake to avoid

The request reads as seven features. Built as seven features it is seven tool
modes, seven undo stories, seven sets of keys, seven translations, and seven
places where a later feature does not compose with an earlier one — the point
where "select the blue splats *inside this box*" turns out to need a rewrite
because the box tool and the colour tool each own a private answer.

Almost all of it is the same two things over four kinds of document:

**make a set**, and **do something to the set**.

Today's list is about ten ways to make a set and about ten things to do with
one. As features that is a hundred; across a seam it is twenty, and the
twenty-first is a day's work. Everything below follows from taking that seam
seriously.

## The four documents

| document | element | what already exists |
|---|---|---|
| sparse reconstruction | 3D point, image, submodel | `ParsedDataset`, `src/sfm/` (incl. `map/Merge.h`) |
| splat model | one Gaussian | engine scene slot, `checkpoint/SplatPly.h` |
| triangle mesh | vertex, face, component | `meshing::MeshData`, `mesh/MeshImport.cpp` |
| image mask | pixel, shape | `app::FrameMask`, `SegmentPanel` |

Each already has a reader, a renderer and a writer. What none of them has is a
**mutable document with a history**. That object is the whole of phase 1, and
the four kinds differ only in what an element is.

## The four objects

### `EditDoc` — what is open, and what has been done to it

One per pane. It holds the loaded original, an ordered list of operations, the
current selection, and a dirty flag. The decision that matters:

> An edit is an entry in a list, not a mutation of the loaded data.

Replaying the list from the original is how undo works, how "save the edits,
not the result" works, and how the same edits survive the model being retrained
underneath them. A 15M-splat model is about a gigabyte of device memory; a
snapshot per undo step is not affordable, and a list of ops is a few hundred
bytes per step. Where replay is expensive, keep a periodic snapshot in the
on-disk cache — the checkpoint machinery already writes and reads splat PLYs —
and replay from the nearest one.

### `Selection` — a set, owned by the document and not by any tool

A bit per element, device-resident for the splat and mesh documents, with a
host mirror pulled only when something needs a count or a histogram. Make it a
`uint8` weight rather than a bit from the start: a soft edge costs the same
memory as a hard one, and the training-region-of-interest feature below wants a
weight anyway.

Every tool writes into the *same* selection through a combine mode — replace,
add, subtract, intersect — bound to the usual modifiers. That single decision
is what makes "box, minus a brush stroke, intersected with *blue and low
opacity*" work without the box tool and the colour tool knowing about each
other, and it is the reason the histogram selector needs no special case for
"inside this region".

### `Tool` — a modal state machine over viewport input

Exactly one active at a time:

```c++
struct Tool {
    virtual void on_enter(EditCtx&) {}
    virtual Consumed on_event(const InputEvent&, EditCtx&) = 0;
    virtual void draw_overlay(ImDrawList*, const EditCtx&) {}
    virtual const Msg& status_hint() const = 0;   // the strip at the bottom
};
```

Camera navigation is itself the default tool. That is what keeps "does this
drag orbit or lasso?" from being a question each feature answers for itself.

### `Op` — the only thing allowed to change a document

`apply`, `undo`, a name (a `Msg`, because it appears in the undo menu) and a
serialized form. Nothing else writes to the document. An op script is then a
file, which is what makes the whole thing testable without a window.

## Making a set

**A 2D region, extruded.** Box, ellipse, lasso, polygon and brush are one
thing: a screen-space stencil. The tool rasterizes its shape into a bitmask on
the CPU — that part is cheap and different per shape — and one kernel does the
rest, projecting each element with the current view matrix and testing the
bitmask. One kernel, every shape, both backends.

Two modifiers make it usable rather than a demo: *front-most only*, against the
depth the render already produced, and a depth range taken from two clicks.
Without them, a lasso around a chair also takes the wall behind it, which is
the first thing anyone tries.

A splat is not a point, so the test needs a policy: by centre, or by any part
of the projected extent. The projection kernel already computes that extent;
offer both and default to the centre.

**3D primitives.** An oriented box with a gizmo, a sphere, a half-space from a
plane. These are what "crop the scene" actually means, and unlike a screen
region they survive a camera move, which makes them the right thing to *store*
on a document as a reusable clip.

**Attribute predicates — the histogram selector.** Any per-element scalar
becomes a brushable histogram: opacity, largest and smallest scale, the
anisotropy the `erank` regularizer already computes from the scales, the DC
colour in a chosen space, distance to the nearest training camera, the
accumulated gradient densification already tracks, the observation count of a
sparse point. Two of them at once is a 2D density plot with a rubber-band box.

Two kernels serve all of it: reduce an attribute into 256 bins over an optional
mask, and threshold it back into the selection. The attributes go in one table
with a name, a getter and a suggested scale, so the panel is generated from the
table rather than written once per attribute.

The "select the blue-white sky splats tangled into the tree branches" case is
this composed with a region, and it needs nothing new — paint roughly over the
tree with the brush, then *intersect* with a colour and opacity box. That
composition is the feature; neither half is.

**Connected components.** A grid-backed union-find over element positions.
This is "remove the floaters" and it is also the machinery the sparse-model
split below needs.

**Grow, shrink, smooth.** A k-NN dilation of the selection. Small, and the
difference between a brush selection that is usable and one that is not.

## Doing something with it

Delete. Isolate (keep only). Transform. Recolour, set opacity, clamp scale.
Export the subset. Lock, so densification and later edits leave it alone.

Two are worth more than the rest:

**Assign to a group.** A named, saved selection. This is what "segment the
model into components" means in practice, it is what makes a selection
reusable, exportable and re-editable, and it gives the histogram and the region
tools something to write to that outlives the click.

**Set a training weight.** The region-of-interest ask is not an edit of a
model, it is an input to the *next run*, and it should not be stored in a splat
file. A group exported as an index list with a weight, named by a training
flag, keeps it where it belongs. Note the constraint that comes with it: the
indices only mean anything as long as densification has not renumbered
everything, so the weight has to be carried as a spatial region or re-derived
per run, not as a list of integers that silently rots.

## Transform, Blender-style

Two layers, and they are different features:

- the **gizmo**: axis and plane handles drawn in the viewport;
- the **modal operator**: `G` / `R` / `S`, then `X` / `Y` / `Z` (twice for the
  complementary plane), typed numbers, `Shift` for precision, `Ctrl` for snap,
  `Enter` or left-click to confirm, `Esc` or right-click to cancel.

Ship the modal operator first. It is the grammar experienced users actually
want, and it is the *easier* of the two: a small state machine over key events
with a live preview and no hit-testing at all.

Say the pivot and the axis frame once, in the transform context — median point,
3D cursor, bounding-box centre, individual origins, in global, local or view
axes — and every tool inherits them.

**Where a transform lives.** `ViewportPanel::set_model_transform` already
places a whole model by moving the *camera* instead of the geometry
(`docs/notes/compare-view.md`), which costs nothing and is the right preview
path. It stops working the moment a transform applies to a subset. So: a
whole-model placement stays camera-side until it is saved, and a subset
transform is an op that rewrites the elements.

For splats that rewrite is `mean -> sRx + t`, `quat -> Rq`, `log scale -> +log
s` — and the spherical harmonics have to be **rotated** with the model, band by
band, or the view-dependent colour swims as the object turns. That is the one
part of "just rotate it" that is real work rather than three lines, and it is
the same rotation a dataset-level pose normalization would want.

## The 2D half: masks, the pen tool, intelligent scissors

`app::FrameMask` is further along than it looks. It already holds an *ordered*
list of keep/remove ellipses and rectangles normalized to the frame, plus an
image stencil intersected with them, and `SegmentPanel` already drags those
shapes over a real decoded frame. Four things are missing:

- **A path shape.** A closed polygon or Bezier with the same keep/remove
  semantics, in the same ordered list: one more `MaskShape::Kind`, one more
  case in `parse_mask_shapes`, and a scanline fill in
  `rasterize_frame_mask`.
- **Livewire (intelligent scissors).** Dijkstra over a cost image built from
  gradient magnitude and direction; the user drops anchors and the path snaps
  to the edge between them. It is a couple of hundred lines over an image the
  panel has already decoded, it runs on the CPU at preview resolution, and the
  cost image is computed once per frame shown.
- **A paint layer.** `FrameMask::image` is already a stencil image, so the
  storage exists; what is new is a canvas to paint on and a convention for
  where the PNG is written next to the dataset.
- **Refining an AI mask.** The composition rule is already "shapes ∩ image".
  Keep the model's output immutable and store the corrections as a separate
  layer composed on top; a correction that is destroyed by re-running the model
  is a correction nobody will make twice.

One data-model change is unavoidable: today a stencil belongs to a *camera*,
and hand corrections belong to a *frame*. Both have to exist, keyed the way
`DatasetPrep` already keys cameras.

## Camera trajectories and video export

A trajectory is keyframes (pose, field of view, time, and the render options —
a fly-through that changes buffer halfway is a legitimate thing to want), an
interpolation (Catmull-Rom on position, slerp/squad on rotation, with an
optional constant-speed reparameterization so a dense cluster of keyframes does
not crawl), and a render job.

Almost everything is already here: `NavCamera` for the pose, `RenderWorker` for
the frames, `app/WriterPool.h` for encoding them off the render thread, and an
ffmpeg dependency the app already shells out to. What is missing is UI — a
timeline strip, "key the current view", a curve drawn in the viewport, a scrub
— plus a JSON file stored next to the model so the same move can be re-rendered
after the model is retrained. The turntable and orbit presets are the same
object with the keyframes generated.

## Splitting and merging a sparse reconstruction

`src/sfm/map/Merge.h` was written for this: `alignReconstructions`, `mergeInto`
and `MergeSession` are separated, in its own words, *because a GUI will drive
them individually*. So merge is mostly wiring plus a preview of the alignment
before it is committed.

Split is the new half, and it needs a stated rule rather than an implementation:
an image belongs to a submodel as much as a point does, and a cut leaves tracks
straddling it. Decide once — a track follows the majority of its observations,
ties are dropped — and show the surviving track count *before* the cut is
applied. A destructive operation whose result is a number nobody can predict
needs that number on screen first.

## Undo, and what it costs

Start with the cheap trick: **deletes are soft**. A deleted element is marked,
not removed, and compaction happens at save. Undo of a delete is then one bit,
hide and isolate become the same op with a different flag, and the machinery is
the selection bitset that already exists. The cost is that the document carries
its garbage until it is written out, which is the right trade.

For the ops that cannot be soft, carry the inverse where it is cheap (any
transform) and a compact delta where it is not. Size the delta honestly before
choosing: one splat at SH degree 3 is 59 floats — 236 bytes — so a one-million
splat delta is 236 MB, which belongs in a disk-backed spill, not in RAM behind
a menu the user does not know is holding it. Cap the history by count *and* by
bytes, show what it is holding, and never let it be the reason a session runs
out of memory.

## Where the code goes

```
src/app/gui/edit/
  EditDoc.{h,cpp}     the document, the op list, undo/redo
  Selection.{h,cpp}   the mask, combine modes, named groups
  Tool.h              the tool interface and the active-tool stack
  tools/              SelectBox, SelectLasso, SelectBrush, Transform, Pen, ...
  Ops.{h,cpp}         delete, transform, recolour, assign to group, ...
  Gizmo.{h,cpp}
  Attributes.{h,cpp}  the per-element scalar table + the brushable histogram
  Trajectory.{h,cpp}
src/engine/EngineEdit.cpp    engine_select_*, engine_apply_transform, ...
src/shaders/select.slang     the stencil test and the attribute reduction
```

Rules already in force that this work has to obey, listed because each one is
cheaper to follow than to retrofit:

- Selection and transform kernels are **Slang, on both backends**. Nothing here
  may become CUDA-only (`AGENTS.md`, "the two-backend rule").
- Every visible string is a `Msg`: a tool name, a status hint, and an op name
  as it appears in the undo menu — which means it is a sentence with a `{0}`,
  never fragments concatenated.
- A **keyboard shortcut is not interface copy**. `G` / `R` / `S` stay `G` /
  `R` / `S` in every language, exactly as `--sh-degree` does.
- Any new setting that gets saved needs its row in the preset field table, or
  it will save, load, and quietly run at its default.
- The engine is a process-global singleton and `CompareView` holds one mutex
  for every pane. An edit runs under that mutex, between renders, like
  everything else that touches the engine.

## Order of work

Each phase is shippable on its own and reuses the previous seam rather than
widening it.

1. **The spine.** `EditDoc`, `Selection`, `Op`, undo, soft delete, one
   box-select tool, "delete selection", "save as" — on the splat document in
   the viewer screen only. This is already the most-asked-for cleanup
   workflow, and everything later is an addition to a working thing.
2. **More ways to select.** Lasso, polygon, brush, invert, grow/shrink, the
   depth modifiers. Nothing else changes.
3. **The histogram panel and named groups.**
4. **Transform.** The modal operator, then the gizmo, then SH rotation, then
   baking a placement on save.
5. **The mask editor.** Path shape, livewire, paint layer, per-frame
   corrections.
6. **Trajectories and video export.**
7. **Sparse split/merge, and region-of-interest weighting for training.**

## What will bite

- **The engine singleton.** A scene-slot model is not the training world. Keep
  an edit to one from reaching the other, or a run inherits it.
- **Editing then continuing training is not free.** A scene slot carries no
  optimizer state, so resuming training on an edited model means resizing the
  Adam moments and the densification arrays in the same order as the splats.
  `checkpoint/Adapt.cpp` already does host-side layout adaptation on resume;
  that is the code to reuse rather than a second one.
- **i18n volume.** An editing UI is a few hundred new messages across thirteen
  languages, and the embedded CJK faces are subset to the characters the
  catalogs use — a new character with no font regeneration is a hollow box
  mid-sentence.
- **ImGui identity.** The viewport is one item and every tool overlay shares
  its ID space; push an ID per tool, or two tools' handles collide.
- **Picking.** `RenderWorker` already returns the 3D point under a pixel from
  the ray-depth channel it downloads anyway. That is the 3D cursor and
  click-to-place; a second picking path would be a second answer to the same
  question.
- **Mouse conventions.** The viewport already orbits on the left button, which
  collides with Blender's "left confirms, right cancels" in a modal operator.
  Settle it once: with no tool active navigation keeps its buttons, and an
  active tool owns both buttons for its whole lifetime.
- **Discoverability.** A modal grammar is invisible. A status strip naming the
  active tool and its two or three keys is not decoration — for this style of
  UI it is the feature.

## Testing it

Two levels, and the cheaper one should carry most of the coverage.

An op list is serializable, so "apply this op script to this model and compare
the result" is a golden-file test that needs no window and no GPU beyond the
one the kernels run on. Every selection producer and every operation is
testable that way.

The interaction itself — that a drag in the viewport produces the selection the
user meant — needs the real window, and that is what
[gui-automation.md](gui-automation.md) is for: the harness drives the tools
through the same input path a user does and hands back the framebuffer, so a
tool can be exercised and eyeballed without a human at the keyboard.
