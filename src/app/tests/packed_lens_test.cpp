// packed_lens -- how a 360 camera's one-image frame is read (app/Pano360.h):
// 2:1 is two fisheye circles, 1:1 is one, and anything else takes the nearer
// shape rather than failing -- so the shape test and the cut are asserted.

#include "app/Pano360.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "BAD ", what.c_str());
    if (!ok) g_failures++;
}

void shape(int w, int h, int lenses, bool exact) {
    bool e = !exact;
    const int got = app::packed_lens_count(w, h, e);
    expect(got == lenses && e == exact,
           std::to_string(w) + "x" + std::to_string(h) + " is " +
               std::to_string(lenses) + (exact ? " lens(es), exactly" : " lens(es), nearest"));
}

}  // namespace

int main() {
    shape(11904, 5952, 2, true);   // an X5 .insp
    shape(1664, 832, 2, true);     // its .lrv
    shape(2880, 2880, 1, true);
    shape(1920, 1080, 2, false);   // 1.78: nearer 2:1
    shape(1440, 1080, 1, false);   // 1.33: nearer 1:1
    shape(1080, 1920, 1, false);

    // 4x2, two channels: the byte is x*10+y, and the channel index is added.
    const int w = 4, h = 2, ch = 2;
    std::vector<uint8_t> px(w * h * ch);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < ch; c++) px[(y * w + x) * ch + c] = (uint8_t)(x * 10 + y + c);
    for (int lens = 0; lens < 2; lens++) {
        std::vector<uint8_t> out;
        int ow = 0;
        app::packed_lens_crop(px.data(), w, h, ch, 2, lens, out, ow);
        bool same = ow == 2 && out.size() == (size_t)(ow * h * ch);
        for (int y = 0; same && y < h; y++)
            for (int x = 0; x < ow; x++)
                for (int c = 0; c < ch; c++)
                    same = same && out[(y * ow + x) * ch + c] ==
                                       (uint8_t)((x + lens * ow) * 10 + y + c);
        expect(same, "lens " + std::to_string(lens) + " is its own half");
    }

    if (g_failures == 0) std::printf("packed_lens_test: OK\n");
    return g_failures == 0 ? 0 : 1;
}
