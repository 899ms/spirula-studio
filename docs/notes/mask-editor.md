# Correcting masks by hand

`src/app/gui/mask/` is a window for fixing the masks a dataset run wrote,
one frame at a time. A correction is kept beside the dataset, not baked into
`masks/`, so when masking runs again the new masks come out with the
corrections re-applied instead of losing them.

## Opening it

**Correct masks** appears in two places once the dataset has masks: on the
dataset screen, and on the Train screen next to the dataset path. The editor
opens on the first frame; the frame slider, `<` and `>` move between frames.
A frame is saved when you leave it, when you close the window, or on **Save**
(Ctrl+S). **Revert frame** puts the run's mask back and deletes that frame's
corrections; **Revert all** does the same for every frame, after asking.

## Tools

On macOS, Ctrl below means Command, as elsewhere in the app.

| tool | key | what it does |
|---|---|---|
| Box, Ellipse, Lasso, Polygon, Brush | `B` `E` `L` `P` `C` | the 3D editor's shapes, painted into the mask |
| Eraser | `X` | the brush with the modes swapped: a plain drag keeps |
| Path | `I` | pen with livewire: each anchor snaps to the edge |
| SAM | `G` | click an object, or type what to drop |

Shapes, brush and path paint with the 3D editor's selection grammar:
a plain or Shift drag **drops**, Ctrl **keeps**, Shift+Ctrl **clears** the
correction back to what the run wrote. The modifiers are read when the shape
completes. Right click or Enter closes a polygon; Ctrl+Z takes back its last
point. Esc cancels a shape in progress.

**Brush size** is in mask pixels, 1 to 4096, and shared by the brush and the
eraser. The slider shows while either is active; `[` and `]` step it, and
Alt+wheel over the picture changes it continuously.

**Path.** Click along an edge to drop anchors; the segment from the last
anchor follows the lowest-cost edge path to the cursor (Mortensen and
Barrett's intelligent scissors). Click the first anchor, press Enter or right
click to close and paint the inside. Hold Ctrl (keep) or Shift+Ctrl (clear)
when you place the first anchor. Ctrl+Z removes an anchor; Esc cancels.

**SAM.** Needs a masking checkpoint; the strip shows the dataset screen's
model picker and download. A click drops the object under it, Ctrl+click
keeps it, Shift+Ctrl+click clears its corrections, and a right click marks a
part as "not this". Further clicks on the same object refine it in place, so
one undo removes the whole object. The object list works as on the dataset
screen. **Text prompt**: type phrases separated by `;` and press Enter or
**Find** to drop every match on the frame; **Common subjects** opens the
subject palette, whose exceptions protect what they name. SAM 2 checkpoints
have no text encoder, so the field is disabled. **Extra margin around what's
removed** grows drops (never keeps or clears) by a fraction of the object's
size; releasing the slider re-applies it to the last drop while that is still
the newest edit. Esc cancels a prompt once its current step finishes.

## Viewing

- **Views**: Overlay (dropped pixels tinted over the photo), Mask only, and
  Side by side (photo left, mask right; both take strokes and clicks). `V`
  cycles them.
- **Peek**: hold Tab over the picture for the bare photo, Shift+Tab for the
  bare mask. Side by side, Tab turns the mask pane into the overlay and
  Shift+Tab the photo pane.
- Wheel zooms; middle drag or Space+drag pans.
- Left/Right step a frame, PageUp/PageDown ten, Home/End go to the ends.
  Frame keys wait while a shape is half drawn.

## Propagate

Copies the open frame's corrections, not its mask, to **Next frame**, a
**Range** (From/To, frame numbers as the status line counts them), or the
**Whole camera**. Only frames of the same camera are touched, and each keeps
its own model output underneath. It replaces the corrections a target already
had, and it copies pixels: it does not follow an object that moves. A target
whose mask has another size is refused and named. **Undo propagate** puts
every touched frame back; it is offered while the source frame is still open.

## Find missing

A frame is missing when it has no mask file or its kept fraction falls
outside **Min kept %** to **Max kept %** (5% to 98% by default): near 0 is an
inverted mask, near 100 one that masked nothing. A background scan counts
them when the editor opens. **Next missing** / **Previous missing**, or `M` /
Shift+`M`, jump between them; **First** and **Last** go to the ends.

## Slideshow

**Play** shows every frame's mask tinted over its photo at the chosen rate,
5 to 30 fps, so a bad frame stands out at speed. Any key or click stops it on
the frame shown, which then opens for editing. A rate the decoder cannot
keep up with shows as a lower rate, never as skipped frames. Starting it
saves the open frame and unloads SAM.

## On disk

Corrections live in `<dataset>/mask_edits/`, which mirrors `masks/`:

| file | holds |
|---|---|
| `<key>.base.png` | a byte copy of `masks/<key>.png` as the run wrote it, made on the first save |
| `<key>.drop.png` | 255 where the user forced drop |
| `<key>.keep.png` | 255 where the user forced keep |
| `index.json` | the mask folder and its polarity, and per frame the FNV-1a fingerprints of the base and of the last composite, the kept fraction and the save time |
| `kept.json` | find missing's cache of kept fractions, keyed by the mask's fingerprint |

`<key>` is the image's path under the image folder without its extension
(`cam0/00023`). The mask written into `masks/<key>.png` is

    final = keep ? 255 : drop ? 0 : base

in the app's polarity, **255 = keep**. A dataset opened with **Flip masks**
(255 = drop) is edited in that convention: bases are read, and composites
written, flipped. `index.json` records which convention and which mask folder
the corrections were made against, and the editor refuses to open them
against another.

When a dataset run re-masks, the frames in `index.json` whose mask is a new
picture get it as their new base, and the corrections are composed over it.
A mask regenerated from the command line (`spirula sam`) is re-composed the
next time its frame opens. `spirula sam mask --shape` stencils also accept
`path x,y,x,y,x,y,...`, a closed polygon of three or more corners.

## Design

```
MaskLayer     the files above: save, revert, re-compose, propagate, kept.json
MaskDoc       one frame in memory: base, two layers, composite, undo history
MaskWindow    the view: pane <-> mask mapping and the window texture's pixels
MaskSession   the frames, the worker, the scan, the open frame, SAM, slideshow
MaskPanel.cpp the window: canvas, tools, status, keys; the only GL
PathTool / Livewire   the pen and its edge search
MaskAdd / MaskSam     SAM results to stencils / the checkpoint and its job thread
MaskSlideshow the slideshow's decoder ring and clock
```

`GuiApp` owns one `MaskSession` and opens it from the two buttons.
`core/PolygonFill.h` (moved out of `edit/SelectShape.cpp` so the CLI can
fill a path) and `core/MaskMargin.h` (moved out of `sam/MaskDilate.cpp` so
the editor shares the masker's margin) are moves, not rewrites. The object
list, margin slider and model picker moved from `SegmentPanel` and `GuiApp`
into `MaskPrompt`, so both screens draw the same controls.
Undo keeps the RLE of each stroke's rectangle, capped at 96 steps or 256 MB.

**Threads.** The UI thread owns the document and every GL call. A single
**worker** loads, saves, reverts and propagates, in order; the UI hands it
snapshots, so painting continues while an 8K frame encodes.
A **scan** thread reads kept fractions for find missing, and discards any
read a worker job overlapped. The slideshow's **prefetch pool** decodes a few
frames ahead into a ring; its thread count comes from a byte budget. SAM
runs on **its own job thread**, one job at a time; each job is stamped with
the frame and a document generation, and a result whose stamp no longer
matches is dropped.

**One live `sam::Session` in the process.** Every inference user shares the
inference layer's one device pool and one unsynchronised stream, and a
dataset run ends by freeing the pool. So the editor yields before anything
else starts inference, refuses prompts while a preview or a run holds the
device, and loads on the device the app froze for inference.

**Data safety.**
- The editor never overwrites `.base.png`. Only a re-mask that produced a
  different picture replaces it; a re-encode of the same pixels does not.
- Propagate snapshots each target's layer files and index entry first. A
  failed write puts that target back, and Undo propagate restores them all;
  a record over 256 MB is not kept, and the user is told.
- Revert keeps a frame's index entry if a layer file cannot be removed, so a
  stray `.drop.png` is never read later as a live correction.
- Every read from and write to the mask folder goes through the recorded
  polarity; in memory everything is 255 = keep.

**What not to undo.**
- A new site that starts inference calls `GuiApp::stop_inference_users()`, not the bare preview close (`GuiApp.cpp:1267`).
- The blocker is pushed every frame, after the screens draw (`GuiApp.cpp:2808`).
- No `sam::Masker` in the editor: under `keep_prompted` its margin is negative and would eat into the object (`MaskSam.cpp:14`).
- Editor clicks live in `MaskSam::prompt()` and never reach the dataset's `MaskSettings`, where an empty `source` means every input (`MaskSam.h:89`).
- Closing mid-job parks SAM in a retiring slot released from the UI thread, never unloaded on the job thread (`MaskSession.cpp:1163`).
- A job co-owns the frame's pixels through `shared_ptr<const ...>` (`MaskSession.h:481`).
- `_doc_gen` is bumped at the one place a document is installed (`MaskSession.cpp:352`).
- Re-compose compares pictures, not file bytes, before it replaces a base (`MaskLayer.cpp:363`).
- Propagate saves its source inside its own job, so it sees that save fail (`MaskSession.cpp:539`).
- The margin grows drops only (`MaskAdd.h:57`).
- The canvas owns Tab except while a text field has focus (`MaskPanel.cpp:395`).
- The slideshow's decoder count comes from the byte budget, not the core count (`MaskSlideshow.h:59`).

**Measured** on an M5 Pro with 7680x3840 frames: the slideshow holds 16.6 fps
into a 1024 px pane and 13.4 fps into 4096 px; resident memory 10 s into
playback is +244 MB over the Train screen (median of three launches).

## Tests and checks

```
./build_develop.bash -DSS_BACKEND=vulkan
build/mask_doc_test       # layer, document, window, session, SAM seam, slideshow
build/frame_mask_test     # stencil shapes, spelling and fill
build/dataset_prep_test   # a re-run re-applies corrections; mask_edits/ is no camera
build/mask_dilate_test    # the masker's margin after the move
```

None of them needs a model or a GPU: `mask_doc_test` builds without
`SS_BUILD_SAM` and stands in for SAM jobs. `SS_MASK_BENCH=<dir>` makes it
write an 8K fixture there and print timings. `build_develop.bash` also runs
`tools/check_sam_guard.sh` (no `sam/` include in the editor outside
`#ifdef SS_BUILD_SAM`) and `tools/mask_editor_checks/survivors.sh`, which
pins `MaskPanel.cpp`'s gates as text, since that file has no unit seam. The
scripts in `tools/mask_editor_checks/` drive the running app through
[GUI automation](gui-automation.md) (macOS): `launch.sh` and `stop.sh` start
and stop an isolated instance, `battery.sh` checks SAM's load, unload,
cancel and stale results, and `memory9.sh` measures slideshow memory.
