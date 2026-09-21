// FrameSink.cpp -- see FrameSink.h.

#include "app/gui/render/FrameSink.h"

#include "app/gui/Subprocess.h"

#include "external/stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace gui::render {

namespace {

// A bounded queue drained by `threads` workers, each frame tagged with its
// number so image files can be written out of order.
class Queue {
public:
    struct Item { int index; std::vector<uint8_t> px; };

    void start(int threads, std::function<bool(Item&)> work, size_t limit) {
        _work = std::move(work);
        _limit = limit;
        for (int i = 0; i < threads; i++) _threads.emplace_back([this] { run(); });
    }
    bool push(Item&& it) {
        std::unique_lock<std::mutex> lk(_mu);
        _space.wait(lk, [&] { return _q.size() < _limit || _failed || _stop; });
        if (_failed || _stop) return false;
        _q.push_back(std::move(it));
        _ready.notify_one();
        return true;
    }
    // Drain (or drop, when `drop`) and join.
    void close(bool drop) {
        {
            std::lock_guard<std::mutex> lk(_mu);
            _closing = true;
            if (drop) { _q.clear(); _stop = true; }
        }
        _ready.notify_all();
        _space.notify_all();
        for (auto& t : _threads) t.join();
        _threads.clear();
    }
    bool failed() const { return _failed.load(); }
    int done() const { return _done.load(); }

private:
    void run() {
        for (;;) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(_mu);
                _ready.wait(lk, [&] { return !_q.empty() || _closing; });
                if (_q.empty()) return;
                it = std::move(_q.front());
                _q.pop_front();
            }
            _space.notify_one();
            if (_failed) continue;
            if (!_work(it)) {
                _failed = true;
                _space.notify_all();
            } else {
                _done++;
            }
        }
    }

    std::function<bool(Item&)> _work;
    size_t _limit = 4;
    std::vector<std::thread> _threads;
    std::mutex _mu;
    std::condition_variable _ready, _space;
    std::deque<Item> _q;
    bool _closing = false, _stop = false;
    std::atomic<bool> _failed{false};
    std::atomic<int> _done{0};
};

class ImageSink : public FrameSink {
public:
    ImageSink(std::string path, int w, int h, bool jpeg, int quality, bool alpha)
        : _path(std::move(path)), _w(w), _h(h), _jpeg(jpeg), _quality(quality),
          _c(alpha && !jpeg ? 4 : 3) {
        const int threads = std::clamp((int)std::thread::hardware_concurrency() - 2, 1, 6);
        _q.start(threads, [this](Queue::Item& it) { return write(it); },
                 (size_t)threads * 2);
    }
    ~ImageSink() override { _q.close(true); }
    bool push(std::vector<uint8_t>&& px) override {
        return _q.push({_next++, std::move(px)});
    }
    bool finish() override {
        _q.close(false);
        return !_q.failed();
    }
    void cancel() override { _q.close(true); }
    std::string error() const override {
        std::lock_guard<std::mutex> lk(_err_mu);
        return _error;
    }
    int channels() const override { return _c; }
    int written() const override { return _q.done(); }

private:
    bool write(Queue::Item& it) {
        std::string file = _path;
        const size_t at = file.find("%05d");
        if (at != std::string::npos) {
            char num[16];
            std::snprintf(num, sizeof num, "%05d", it.index + 1);
            file.replace(at, 4, num);
        }
        const int ok = _jpeg
            ? stbi_write_jpg(file.c_str(), _w, _h, _c, it.px.data(), _quality)
            : stbi_write_png(file.c_str(), _w, _h, _c, it.px.data(), _w * _c);
        if (!ok) {
            std::lock_guard<std::mutex> lk(_err_mu);
            if (_error.empty()) _error = file;
        }
        return ok != 0;
    }

    std::string _path;
    int _w, _h;
    bool _jpeg;
    int _quality, _c;
    int _next = 0;
    Queue _q;
    mutable std::mutex _err_mu;
    std::string _error;
};

class PipeSink : public FrameSink {
public:
    PipeSink(const std::vector<std::string>& argv, int w, int h) : _w(w), _h(h) {
        _ok = _proc.start(argv, [this](const std::string& line) {
            std::lock_guard<std::mutex> lk(_log_mu);
            _tail.push_back(line);
            if (_tail.size() > 12) _tail.pop_front();
        });
        if (!_ok) _error = argv.empty() ? std::string() : argv[0];
        // One writer: the encoder takes frames in order.
        _q.start(1, [this](Queue::Item& it) {
            return _proc.write(it.px.data(), it.px.size());
        }, 3);
    }
    ~PipeSink() override {
        _q.close(true);
        _proc.kill();
    }
    bool push(std::vector<uint8_t>&& px) override {
        if (!_ok) return false;
        return _q.push({0, std::move(px)});
    }
    bool finish() override {
        _q.close(false);
        if (!_ok) return false;
        const int code = _proc.finish();
        _ok = false;
        if (code != 0 || _q.failed()) {
            std::lock_guard<std::mutex> lk(_log_mu);
            for (const std::string& l : _tail) _error += (_error.empty() ? "" : "\n") + l;
            if (_error.empty()) _error = "exit " + std::to_string(code);
            return false;
        }
        return true;
    }
    void cancel() override {
        _q.close(true);
        _proc.kill();
        _proc.finish();
        _ok = false;
    }
    std::string error() const override { return _error; }
    int channels() const override { return 3; }
    int written() const override { return _q.done(); }

private:
    int _w, _h;
    ProcessPipe _proc;
    bool _ok = false;
    Queue _q;
    std::mutex _log_mu;
    std::deque<std::string> _tail;
    std::string _error;
};

}  // namespace


std::unique_ptr<FrameSink> open_image_sink(const std::string& path, int width,
                                           int height, bool jpeg, int quality,
                                           bool alpha) {
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return std::make_unique<ImageSink>(path, width, height, jpeg, quality, alpha);
}

std::unique_ptr<FrameSink> open_pipe_sink(const std::vector<std::string>& argv,
                                          int width, int height) {
    return std::make_unique<PipeSink>(argv, width, height);
}

std::vector<std::string> encoder_argv(const Encoder& e, int width, int height,
                                      double fps, bool h265, int quality,
                                      bool spherical, const std::string& path) {
    char size[32], rate[32];
    std::snprintf(size, sizeof size, "%dx%d", width, height);
    std::snprintf(rate, sizeof rate, "%.6g", fps);
    quality = std::clamp(quality, 0, 2);
    if (e.kind == Encoder::BuiltIn) {
        std::vector<std::string> a = {e.exe, "encode", "--size", size,
                                      "--fps", rate, "--codec", h265 ? "h265" : "h264",
                                      "--quality", std::to_string(quality)};
        if (spherical) a.push_back("--spherical");
        a.push_back("-o");
        a.push_back(path);
        return a;
    }
    // x264 and x265 put the same look about four CRF apart.
    static const int kCrf264[3] = {16, 20, 26}, kCrf265[3] = {20, 24, 30};
    std::vector<std::string> a = {e.exe, "-hide_banner", "-loglevel", "error",
                                  "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                  "-s", size, "-r", rate, "-i", "-",
                                  "-c:v", h265 ? "libx265" : "libx264",
                                  "-preset", "medium",
                                  "-crf", std::to_string(h265 ? kCrf265[quality] : kCrf264[quality]),
                                  "-pix_fmt", "yuv420p"};
    if (h265) { a.push_back("-tag:v"); a.push_back("hvc1"); }
    a.push_back("-movflags");
    a.push_back("+faststart");
    a.push_back(path);
    return a;
}

}  // namespace gui::render
