# Rendering a photo or a video

Render mode turns a model on the viewer screen into a picture or a film: a
camera move laid out as keyframes, a lens, the models to show and how the
picture passes from one to the next, and a file at the end. It is phase 6 of
[gui-editing-plan.md](gui-editing-plan.md) and lives in `src/app/gui/render/`;
the GPU video encoder it feeds is `src/video/VideoEncoder.cpp`.

```
RenderProject    the move, the lens, the output, the shots: one JSON file
Trajectory       keys -> a camera at any time
FrameRenderer    one frame: every model drawn by its own renderer, composited
FrameSink        where frames go: image files, or an encoder process
RenderSession    the mode: viewport interaction, preview, playback, export
RenderPanel.cpp, RenderTimeline.cpp    its panel and its timeline
```

## Getting in

The viewer screen has a **Render photo or video** button on its toolbar and a
**Render** button on every pane; the pane's model becomes the primary one, and
every other open model is a source a shot can show. The meshing preview is a
comparison, not a stage, and does not offer it. A finished training run offers
**Render photo or video** and **Edit the model** under *Train again*: both open
the run's model on the viewer screen and go straight into the mode.

Editing and rendering switch into each other from either panel. The editor
stays open behind the render, so what it deleted or moved is what is filmed.

## Coordinates

Poses are stored in the **primary model's saved coordinates**: its file's
frame with the editor's placement applied. That is the frame a model trained
from the same dataset shares, so a project made on one run re-renders on the
next. Every source is taken to share it (a dataset, its splats and a mesh made
from them do), which is why the render ignores the comparison view's
per-pane alignment and uses each model's own file frame instead.

A project records the placement it was laid out against. Opened against
another placement of the same model it is carried across the difference, and
a model saved moved in the editor gets moved copies of its projects beside it
(`copy_moved_projects`) while the originals stay with the original.

*Up* comes from the dataset when there is one (the parsers' levelling guess,
`ParsedDataset::normalized_rotation`, found through the run's `config.json`
and moved by its `scene_transform.json`). A model nothing levels offers *Take
up from the view*. Up decides how an orbit turns, how an aimed camera stays
level and which way the sweep travels. An orbit started from a view steeper
than 60 degrees is tipped to 30, since from overhead it would only spin.

## The motion

Every channel -- position, rotation, look-at target, roll, log focal length --
runs on one cubic Hermite spline in **time**, so a hold or an eased end stops
all of them together:

- tangents are the time-weighted Catmull-Rom slope, scaled by `1 - tension`;
- zero at a key marked *Stop here*, and at the ends when *Start and stop
  gently* is on;
- limited on the whole vector (position, target): no faster than three times
  the slower neighbouring chord, and easing off towards rest as the path
  doubles back. Without it, unevenly timed keys send the camera past a key and
  back; limiting per component instead would make a turned path move
  differently from the path turned, which `render_project_test` checks.

Rotations are the normalized spline of hemisphere-aligned quaternions. Between
two aimed keys the rotation is instead derived from the interpolated position,
target and roll, which is what keeps an orbit on its subject. *Constant speed*
is a warp of time by arc length (a 48-samples-per-segment table), with the
easing applied to the warp. The projection and the distortion tier cannot be
blended and change at the key; the zoom glides.

## One frame

`FrameRenderer` draws each model with the renderer the viewer already uses,
at the output size:

- **splats**: the engine through a `RenderWorker` of its own, in the raw mode
  (`ViewRequest::raw`): no overlays, the lens distortion passed through, and
  RGBA premultiplied over nothing, alpha = 1 - transmittance;
- **points and meshes**: `PreviewRenderer` into its own FBO, cleared
  transparent, with the render styles (`PreviewStyle`): square, circle,
  Gaussian or sphere points, lens distortion in the vertex projection, and a
  clip plane for the sweep.

Two layers at most meet in one GLSL compositing pass: a mix through a mask
(crossfade, wipe, iris), or one over the other (the sweep, where each layer
has cut itself to its side of the level), then two tints (a dip, then the
fades) and the background. Readback is one `glReadPixels` of the result.

Effects that rewrite splats -- *Grow in* scales them up from 2% while they
fade in, *Sweep* hides those past the level with a soft band -- need the
splats host-side. They are read from the file on first use, rewritten per
frame under OpenMP and uploaded with `engine_scene_update` inside the render's
`before_render` hook, under the engine lock, and put back in `after_render`,
so the viewport never sees a half-applied frame. Splats deleted in the editor
stay deleted: the rewrite starts from the editor's survivors.

A mesh under a fisheye or equirectangular lens is projected per vertex, so a
long triangle keeps straight edges where the lens would bend them; the splats
and the points are exact.

## Transitions

A **shot** is a start time, a model (or nothing) and how it arrives:

| | |
|---|---|
| cut, crossfade, dip to black / white | what every editor has |
| wipe left / right / up / down, iris | a moving mask in picture space |
| sweep upward | a level rises along *up*, the new model below it, the old above, the band at the cut lit |
| grow in | the new model's splats swell out of points while the old one fades |

*From dataset to splats to mesh* lays out the three shots that tell a
reconstruction's story. Fade in and fade out are separate from the shots, as
in an editor, and a photo has none.

## Output

A photo, a video, or a folder of numbered frames. PNG and JPEG are written by
`stb_image_write` on a few threads (`FrameSink.cpp`). A video is raw RGB piped
into an encoder process:

- **`spirula encode`**, where the build has it. It is `src/video/`'s encoder,
  so it is **patent-gated** like the decoder (`SS_ENABLE_PATENTED`, off by
  default), and it runs in its own process because it needs a Vulkan device
  of its own beside the engine's (AGENTS.md, "Three Vulkan devices"). The GUI
  runs `spirula encode --probe` once, which encodes two small frames per codec
  and prints the ones that worked.
- **ffmpeg** otherwise (libx264 / libx265, the same three quality steps).
- Neither: frames still work, and the panel says why video does not.

An equirectangular video is tagged as 360 (Spherical Video V1 and V2).

## The file

`<dataset>/renders/*.json` by default -- the dataset the run read, or the
folder beside the model when that already has a `renders/` -- with an
`autosave.json` written when the mode is left with a move nobody saved
(quietly on a switch to the editor, where the move is still in memory). The
format is `"format": "spirula-render"`, `"version": 1`; unknown keys are
ignored and missing ones take their defaults, so it can grow without a
version bump. Sources are listed by path but matched by position: a project
opened with a different set of models shows whatever is open in each slot.

## Testing

`render_project_test` covers the pure half: the path through its keys, holds,
eased ends, constant speed, rigid transforms of the path, aim and roll, the
lens arithmetic, JSON round trips, moved-project copies and the dataset lens
clustering. The rest was driven through the GUI automation surface
([gui-automation.md](gui-automation.md)) on a trained scene with its sparse
model and a mesh: orbit and capture presets, G / R / S on keys, undo, the
three preview layouts, every projection, the story transitions, and PNG, H.264
and H.265 output checked with ffprobe and against a libx264 reference (42-44
dB luma PSNR).
