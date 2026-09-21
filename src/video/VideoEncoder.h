#pragma once
// H.264 / H.265 encoding on the GPU through VK_KHR_video_encode_*: RGB in,
// Annex-B access units out. PATENT-GATED like the rest of src/video/.
//
// One I frame then P frames, each referencing the one before, an IDR every
// two seconds; constant QP where the driver offers it, a VBR target where it
// does not. RGB becomes NV12 in a compute kernel (BT.709, studio range) and
// is copied into the encoder's source picture. src/video/README.md, "Encode".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace video {

struct EncodeOptions {
    bool h265 = false;
    int width = 0, height = 0;
    double fps = 30.0;
    int quality = 1;                    // 0 best, 1 standard, 2 smallest
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    // Needs a Context created with ContextOptions::want_encode.
    bool open(const EncodeOptions& o, std::string& error);
    // One RGB24 frame, rows top-down. `sync` says it starts a GOP.
    bool encode(const uint8_t* rgb, std::vector<uint8_t>& access_unit, bool& sync,
                std::string& error);
    // The parameter sets as the driver wrote them, Annex-B.
    const std::vector<uint8_t>& headers() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace video
