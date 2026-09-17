#pragma once

// Pano360 -- a 360 camera's own frame layout, and the views a dataset wants
// out of it.
//
// One place owns the geometry: the GoPro MAX .360 EAC packing, the plan that
// turns it into equirectangular or a ring of pinhole faces, and the resampler
// both decode paths apply. ffmpeg is asked only to decode and to cut the
// strips out; its own `v360=eac` insets every face by 2 px, which puts a 4 px
// step across the seam between the two tracks. docs/datasets.md records where
// the layout numbers were measured.

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// How a GoPro MAX packs its two tracks: an EAC 3x2 cubemap, faces as tall as a
// track, with a `strip`-wide overlap inserted at the centre line of each
// track's two side faces. That centre line is the lens seam.
struct Eac360Layout {
    int track_w = 0, track_h = 0;
    int face = 0;                 // EAC face side
    int strip = 0;                // inserted at each of the two split points
    bool valid() const { return face > 0; }
    int canvasW() const { return 3 * face; }
    int canvasH() const { return 2 * face; }
};

// Two equal video tracks whose width is three square faces plus two small
// strips: 4096x1344 (5.6K), 2272x736 (3K), 5952x1920 (8K).
bool eac360_detect(int tracks, int width, int height, Eac360Layout& out);

// One column range of a track, and where it lands in the canvas row. Each
// overlap is cut at its middle, which is where the two lenses' copies of the
// seam meet, so a bilinear tap either side of the cut stays on its own lens.
struct Eac360Slice {
    int src_x = 0, dst_x = 0, width = 0;
};
std::vector<Eac360Slice> eac360_slices(const Eac360Layout& l);

// A track pixel's viewing direction, unit, in the frame Pano360View::rot maps
// into. False inside an overlap strip -- the one place the packing stores two
// answers -- and outside the track.
bool eac360_direction(const Eac360Layout& l, int row, float x, float y,
                      float dir[3]);

enum class Pano360Mode { Off, Faces, Equirect };

struct Pano360Options {
    Pano360Mode mode = Pano360Mode::Off;
    int   size = 0;         // face side, or equirect width; 0 = from the layout
    // How the camera was held, degrees. Applied to the whole sphere, so a
    // 180 roll turns an inverted mount's views the right way up.
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
};

// One image per frame. The camera frame is x right, y down, z forward, `rot`
// maps a view-space ray into it, and `fov` is the HORIZONTAL one -- a view
// narrower than it is wide is half a cube face, not a cropped one.
struct Pano360View {
    std::string dir;        // "cam0"...; empty when the plan makes one image
    int   width = 0, height = 0;
    float fov = 0.0f;       // 0 = equirectangular
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row major
};

// Faces mode gives five views per lens -- the cube face on that lens's axis and
// the near half of its four side faces -- so every view is fed by ONE lens.
// docs/datasets.md says why nothing smaller is seam-free.
std::vector<Pano360View> pano360_views(const Eac360Layout& l,
                                       const Pano360Options& o);

// What `size` = 0 resolves to: the face side, or the panorama's width.
int pano360_default_size(const Eac360Layout& l, const Pano360Options& o);

// Where each output pixel reads from the canvas. Built once per view and
// reused for every frame; ~8 bytes a pixel.
struct Pano360Remap {
    int width = 0, height = 0;
    std::vector<float> x, y;
};
void pano360_remap(const Eac360Layout& l, const Pano360View& v,
                   Pano360Remap& out);

// Bilinear gather into `out` (width*height*3 bytes) from a tightly packed RGB
// canvas. `threads` <= 0 uses every core.
void pano360_apply(const Pano360Remap& m, const uint8_t* canvas, int canvas_w,
                   int canvas_h, int threads, uint8_t* out);

// The two tracks' frames laid out as one canvas, tightly packed RGB.
void pano360_canvas(const Eac360Layout& l, const uint8_t* track0,
                    const uint8_t* track1, uint8_t* out);

// Decodes both video streams and cuts the overlap strips out, leaving the
// canvas on `pano360_canvas_pad`. `pre` ("fps=6") runs on the canvas, so
// ffmpeg never scales a frame that selection will throw away.
std::string pano360_graph(const Eac360Layout& l, const std::string& pre);
inline const char* pano360_canvas_pad() { return "canvas"; }

}  // namespace app
