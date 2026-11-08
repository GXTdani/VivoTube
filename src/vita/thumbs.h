// Thumbnail texture cache. Images download and decode (JPEG -> RGBA) on two worker threads;
// textures are created on the main thread. Requests for things no longer on screen are
// skipped, and least-recently-drawn textures are evicted.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "threads.h"

struct vita2d_texture;

namespace vt {

class ThumbCache {
public:
    void start();
    void stop();
    // Main thread. Returns the texture if ready (and marks it used this frame), else queues it.
    vita2d_texture* get(const std::string& url);
    // Main thread, once per frame: uploads finished images and evicts old ones.
    void update();

private:
    enum State { Pending, Loading, Ready, Failed };
    struct Entry {
        State state = Pending;
        vita2d_texture* tex = nullptr;
        uint32_t lastUsed = 0;
    };
    struct Decoded {
        std::string url;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    void workerLoop();

    std::unordered_map<std::string, Entry> entries_;  // main thread only
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, uint32_t> wanted_;  // url -> frame last requested (shared)
    std::unordered_set<std::string> inflight_;          // being downloaded (shared)
    std::vector<Decoded> decoded_;
    std::vector<std::string> failed_;
    std::atomic<uint32_t> frame_{1};
    bool quit_ = false;
    Thread workers_[2];
};

}  // namespace vt
