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
FrameSink        where frames go: image files, a GIF, or an encoder process
GifWriter        animated GIF: a palette per frame, ordered dither, LZW
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

A project records the placement it was laid out against, and the keys follow
the model: turn it in the editor and the move turns with it
(`RenderSession::follow_placement`). A save that writes the placement into
the file is remembered, so the model read back from it -- the same place in
the world, now with no placement -- does not move the keys a second time.
Opened against another placement of the same model a project is carried
across the difference, and a model saved moved gets moved copies of its
projects beside it (`copy_moved_projects`) while the originals stay.

*Up* comes from the dataset when there is one (the parsers' levelling guess,
`ParsedDataset::normalized_rotation`, found through the run's `config.json`
and moved by its `scene_transform.json`). A model nothing levels offers *Take
up from the view*. Up decides how an orbit turns, how an aimed camera stays
level and which way the sweep travels. An orbit started from a view steeper
than 60 degrees is tipped to 30, since from overhead it would only spin.

## The motion

Every channel -- position, rotation, look-at target, roll, log focal length --
is a cubic through the keys on one shared parameter, so a stop or an eased
end stops all of them together. The *Curve* decides the slopes at the keys:

- **Smooth spline (C2)**, the default: the interpolating cubic B-spline, one
  tridiagonal solve per channel, so speed *and* acceleration are continuous
  through every key. Free ends have zero second derivative; a key marked
  *Stop here*, and the ends when *Start and stop gently* is on, have zero
  slope and split the solve there.
- **Catmull-Rom**: the local time-weighted slope, scaled by `1 - tension` and
  limited on the whole vector (no faster than three times the slower
  neighbouring chord, easing off as the path doubles back). Only C1; moving
  a key changes less of the path.
- **Straight lines**.

The parameter is key time, unless *Constant speed* is on: then it is the
distance along the keys, so unevenly timed keys cannot make the spline
overshoot, and time is warped by arc length (a 48-samples-per-segment
table). A **closed loop** adds a knot back at the first key and solves the
spline cyclically (Sherman-Morrison); a rotation that makes a full turn comes
back as -q, which the cyclic system carries as a sign. A loop's video leaves
out its last frame, which would be its first again.

Rotations are the normalized spline of hemisphere-aligned quaternions. Between
two aimed keys the rotation is instead derived from the interpolated position,
target and roll, which keeps an orbit on its subject. The projection and the
distortion tier cannot be blended and change at the key; the zoom glides.

Editing keys keeps the move: a key added between two others goes where the
camera already passes (`add_key`), a deleted or reordered key's neighbours
keep the lens they were seen through (`keep_lenses`), R turns cameras about
their own middle, and *Smooth keyframes* pulls keys towards their neighbours'
line with Taubin's second outward pass so a loop does not shrink.

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
and the points are exact. Splats render with the primitive the viewport shows
them with (3DGUT follows a wide lens where 3DGS smears at the edge) unless a
model's *Render as* says otherwise.

The keys are drawn one size in the world, through the same lens model the
engine renders with (`camhost::generate_ray`): a pyramid, curved by
distortion, a dome for a fisheye, a globe for the whole sphere. The camera's
own picture sits in a resizable corner, side by side behind a splitter, or
fills the view.

## Transitions

A **shot** is a start time, a model (or nothing) and how it arrives:

| | |
|---|---|
| cut, crossfade, dip to black / white | what every editor has |
| wipe left / right / up / down, iris | a moving mask in picture space |
| sweep upward | a level rises along *up*, the new model below it, the old above, the band at the cut lit |
| grow in | the new model's splats swell out of points while the old one fades |

*Show each model in turn* splits the video into one shot per model -- points,
then splats, then meshes -- with the transition that suits each; shots can
then be reordered (models and transitions trade places, times stay). Fade in
and fade out are separate from the shots, as in an editor, and a photo has
none.

## Output

A photo, a video, or a folder of numbered frames. Photos and frames are PNG,
PNG with transparency, or JPEG, written by `stb_image_write` on a few threads
(`FrameSink.cpp`). A video is an MP4 (H.264, H.265 or AV1) or an animated GIF.
GIF is written here (`GifWriter.cpp`): a median-cut palette per frame, an 8x8
ordered dither that does not crawl between frames as error diffusion would,
and frames dropped where a delay would fall under 2/100 s, which players
stretch. An MP4 is raw RGB piped into an encoder process, chosen per size:

- **`spirula encode`** for H.264 and H.265 when the frame fits the device
  (`--probe` reports each codec's largest frame; NVIDIA's H.264 stops at
  4096). It is `src/video/`'s encoder, so it is **patent-gated**
  (`SS_ENABLE_PATENTED`, off by default), and a process of its own because it
  needs a Vulkan device beside the engine's. If it fails, the export starts
  again with ffmpeg.
- **ffmpeg** otherwise, and first for AV1: libx264 / libx265 / SVT-AV1 where
  it has them, a GPU encoder of its own next. The built-in AV1 predicts every
  P frame from its key frame (src/video/README.md, "Encode"), which is
  correct and about 2.5 times the size.
- Neither: frames and GIF still work, and the panel says why the rest does not.

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
eased ends, constant speed, C2 joins against Catmull-Rom's C1, closed loops
(a circle stays a circle within 1%, a looped full turn keeps turning through
the seam), rigid transforms of the path, aim and roll, the lens arithmetic,
JSON round trips including older files, moved-project copies, dataset lens
clustering, and a GIF read back through a decoder of its own. The rest was
driven through the GUI automation surface ([gui-automation.md](gui-automation.md))
on a trained scene with a mesh: orbit loops, insertion, deletion, R, the
lens-aware gizmos, the three preview layouts, keys following a turned model,
3DGS against 3DGUT, shots and model removal, and H.264 at 4946x3286 through
ffmpeg, SVT-AV1, GIF and RGBA frames checked with ffprobe. `spirula encode`
was checked against the source by PSNR: H.264 40, H.265 39, AV1 39 dB.
