// Threads with explicit stack sizes (std::thread cannot set one, and Vita defaults are small),
// plus a job pool whose completion callbacks run on the main thread.
#pragma once

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace vt {

class Thread {
public:
    Thread() = default;
    ~Thread() { join(); }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool start(std::function<void()> fn, size_t stackBytes = 256 * 1024);
    void join();
    bool running() const { return started_; }

private:
    static void* entry(void* self);
    pthread_t thread_{};
    bool started_ = false;
    std::function<void()> fn_;
};

class JobPool {
public:
    ~JobPool() { stop(); }
    void start(int threads, size_t stackBytes = 512 * 1024);
    void stop();

    // work runs on a worker; done (optional) runs on the main thread inside poll().
    void post(std::function<void()> work, std::function<void()> done = nullptr);
    // Main thread: runs finished callbacks. Returns how many ran.
    int poll();
    int pending() const { return pending_.load(); }

private:
    void workerLoop();
    struct Job {
        std::function<void()> work, done;
    };
    std::vector<Thread*> threads_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::mutex doneMutex_;
    std::vector<std::function<void()>> done_;
    std::atomic<int> pending_{0};
    bool quit_ = false;
};

}  // namespace vt
