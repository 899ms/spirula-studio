#pragma once

// Where rendered frames go: image files written by stb on a few threads, or
// raw RGB piped into an encoder process -- `spirula encode` where this build
// has the GPU encoder (patent-gated, src/video/), ffmpeg otherwise.
//
// push() is called from the GUI thread and blocks only while the bounded
// queue is full, which is what keeps a slow disk or encoder from growing it.

#include <memory>
#include <string>
#include <vector>

namespace gui::render {

class FrameSink {
public:
    virtual ~FrameSink() = default;
    // One frame, rows top-down, channels() bytes a pixel. False once the
    // sink has failed; error() then says why.
    virtual bool push(std::vector<uint8_t>&& pixels) = 0;
    // Everything written and closed.
    virtual bool finish() = 0;
    virtual void cancel() = 0;
    virtual std::string error() const = 0;
    virtual int channels() const = 0;
    // What has been written so far.
    virtual int written() const = 0;
};

// PNG or JPEG files: `path` itself for a single photo, else `path` with the
// frame number filled into its one `%05d`.
std::unique_ptr<FrameSink> open_image_sink(const std::string& path, int width,
                                           int height, bool jpeg, int quality,
                                           bool alpha);

// Frames into `argv`'s stdin as rgb24. Its output is kept for the error.
std::unique_ptr<FrameSink> open_pipe_sink(const std::vector<std::string>& argv,
                                          int width, int height);

// Which program turns frames into a video file.
struct Encoder {
    enum Kind { None = 0, BuiltIn, Ffmpeg };
    Kind kind = None;
    std::string exe;
};
// The command for an encode of `width` x `height` at `fps` into `path`.
// `quality` 0 best .. 2 smallest; `h265` picks the codec; `spherical` tags
// an equirectangular video as 360 where the encoder can.
std::vector<std::string> encoder_argv(const Encoder& e, int width, int height,
                                      double fps, bool h265, int quality,
                                      bool spherical, const std::string& path);

}  // namespace gui::render
