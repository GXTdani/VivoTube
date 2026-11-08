#include "threads.h"

#include "../core/util.h"

namespace vt {

void* Thread::entry(void* self) {
    Thread* t = (Thread*)self;
    t->fn_();
    return nullptr;
}

bool Thread::start(std::function<void()> fn, size_t stackBytes) {
    if (started_) return false;
    fn_ = std::move(fn);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackBytes);
    int rc = pthread_create(&thread_, &attr, &Thread::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        VT_LOG("thread: pthread_create failed (%d)", rc);
        return false;
    }
    started_ = true;
    return true;
}

void Thread::join() {
    if (!started_) return;
    pthread_join(thread_, nullptr);
    started_ = false;
}

void JobPool::start(int threads, size_t stackBytes) {
    quit_ = false;
    for (int i = 0; i < threads; i++) {
        Thread* t = new Thread();
        t->start([this] { workerLoop(); }, stackBytes);
        threads_.push_back(t);
    }
}

void JobPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (Thread* t : threads_) {
        t->join();
        delete t;
    }
    threads_.clear();
}

void JobPool::post(std::function<void()> work, std::function<void()> done) {
    pending_++;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(Job{std::move(work), std::move(done)});
    }
    cv_.notify_one();
}

int JobPool::poll() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(doneMutex_);
        ready.swap(done_);
    }
    for (auto& f : ready) f();
    return (int)ready.size();
}

void JobPool::workerLoop() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job.work) job.work();
        pending_--;
        if (job.done) {
            std::lock_guard<std::mutex> lock(doneMutex_);
            done_.push_back(std::move(job.done));
        }
    }
}

}  // namespace vt
