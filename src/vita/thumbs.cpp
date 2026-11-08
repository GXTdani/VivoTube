#include "thumbs.h"

#include <vita2d.h>

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <jpeglib.h>

#include "../core/http.h"
#include "../core/util.h"

namespace vt {

namespace {

const size_t kMaxTextures = 90;
const int kUploadsPerFrame = 3;

// Plain libjpeg API with our own error manager. (The prebuilt TurboJPEG wrapper keeps a jmp_buf
// sized for an older newlib, and setjmp overruns it into the decoder's saved callbacks.)
struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void jpeg_fail(j_common_ptr c) { longjmp(((JpegErr*)c->err)->jump, 1); }
void jpeg_quiet(j_common_ptr, int) {}
void jpeg_no_output(j_common_ptr) {}

bool decode_jpeg(const std::string& data, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (data.size() < 4 || (uint8_t)data[0] != 0xFF || (uint8_t)data[1] != 0xD8) return false;
    jpeg_decompress_struct cinfo;
    JpegErr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    err.pub.emit_message = jpeg_quiet;
    err.pub.output_message = jpeg_no_output;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char*)data.data(), (unsigned long)data.size());
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    // Keep textures small: anything wider than 400px is halved (e.g. 480x360 hqdefault).
    if (cinfo.image_width > 400) {
        cinfo.scale_num = 1;
        cinfo.scale_denom = 2;
    }
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);
    w = (int)cinfo.output_width;
    h = (int)cinfo.output_height;
    rgba.resize((size_t)w * h * 4);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgba.data() + (size_t)cinfo.output_scanline * w * 4;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return w > 0 && h > 0;
}

}  // namespace

void ThumbCache::start() {
    quit_ = false;
    for (Thread& t : workers_) t.start([this] { workerLoop(); }, 256 * 1024);
}

void ThumbCache::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    for (Thread& t : workers_) t.join();
    for (auto& e : entries_)
        if (e.second.tex) vita2d_free_texture(e.second.tex);
    entries_.clear();
}

vita2d_texture* ThumbCache::get(const std::string& url) {
    if (url.empty()) return nullptr;
    uint32_t frame = frame_.load();
    auto it = entries_.find(url);
    if (it == entries_.end()) {
        Entry e;
        e.lastUsed = frame;
        entries_.emplace(url, e);
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_[url] = frame;
        cv_.notify_one();
        return nullptr;
    }
    Entry& e = it->second;
    e.lastUsed = frame;
    if (e.state == Ready) return e.tex;
    if (e.state == Pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto w = wanted_.find(url);
        if (w != wanted_.end()) w->second = frame;
        else wanted_[url] = frame;
        cv_.notify_one();
    }
    return nullptr;
}

void ThumbCache::update() {
    uint32_t frame = frame_.fetch_add(1) + 1;
    std::vector<Decoded> ready;
    std::vector<std::string> failed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = std::min(decoded_.size(), (size_t)kUploadsPerFrame);
        ready.assign(std::make_move_iterator(decoded_.begin()), std::make_move_iterator(decoded_.begin() + n));
        decoded_.erase(decoded_.begin(), decoded_.begin() + n);
        failed.swap(failed_);
    }
    for (Decoded& d : ready) {
        auto it = entries_.find(d.url);
        if (it == entries_.end()) continue;
        vita2d_texture* tex = vita2d_create_empty_texture(d.w, d.h);
        if (!tex) {
            it->second.state = Failed;
            continue;
        }
        uint8_t* dst = (uint8_t*)vita2d_texture_get_datap(tex);
        unsigned stride = vita2d_texture_get_stride(tex);
        for (int y = 0; y < d.h; y++) memcpy(dst + (size_t)y * stride, d.rgba.data() + (size_t)y * d.w * 4, d.w * 4);
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        it->second.tex = tex;
        it->second.state = Ready;
    }
    for (const std::string& u : failed) {
        auto it = entries_.find(u);
        if (it != entries_.end()) it->second.state = Failed;
    }
    // Requests that were dropped while off screen go back to Pending so they can be re-queued.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_)
            if (e.second.state == Pending && !wanted_.count(e.first) && !inflight_.count(e.first) &&
                e.second.lastUsed + 2 >= frame)
                wanted_[e.first] = e.second.lastUsed;
    }
    // Evict least-recently-drawn textures that have not been drawn for two frames (the GPU may
    // still be reading the last frame's textures).
    size_t readyCount = 0;
    for (auto& e : entries_)
        if (e.second.state == Ready) readyCount++;
    while (readyCount > kMaxTextures) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.state != Ready || it->second.lastUsed + 2 >= frame) continue;
            if (victim == entries_.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
        }
        if (victim == entries_.end()) break;
        vita2d_free_texture(victim->second.tex);
        entries_.erase(victim);
        readyCount--;
    }
    // Forget failed / stale pending entries occasionally so the map does not grow forever.
    if (entries_.size() > kMaxTextures * 4) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.state != Ready && it->second.state != Loading && it->second.lastUsed + 600 < frame)
                it = entries_.erase(it);
            else
                ++it;
        }
    }
}

void ThumbCache::workerLoop() {
    while (true) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !wanted_.empty(); });
            if (quit_) return;
            // Newest request first; drop anything not requested in the last few frames.
            uint32_t frame = frame_.load();
            auto best = wanted_.end();
            for (auto it = wanted_.begin(); it != wanted_.end();) {
                if (it->second + 3 < frame) {
                    it = wanted_.erase(it);
                    continue;
                }
                if (best == wanted_.end() || it->second > best->second) best = it;
                ++it;
            }
            if (best == wanted_.end()) continue;
            url = best->first;
            wanted_.erase(best);
            inflight_.insert(url);
        }
        HttpResponse r = http_get(url);
        Decoded d;
        d.url = url;
        bool ok = r.ok() && decode_jpeg(r.body, d.w, d.h, d.rgba);
        std::lock_guard<std::mutex> lock(mutex_);
        inflight_.erase(url);
        if (ok) decoded_.push_back(std::move(d));
        else failed_.push_back(url);
    }
}

}  // namespace vt
