// DASH player for one H.264 video stream + one AAC audio stream.
//
// Threads:
//   loader - fetches init+sidx for both streams, then downloads fragments by byte range,
//            keeping ~30 s buffered ahead of the playhead
//   video  - converts samples to Annex-B and decodes them into a ring of YUV textures
//   audio  - decodes AAC and feeds the audio port; its timestamps are the master clock
// The main thread calls draw() each frame, which shows the newest frame due at the clock.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/fmp4.h"
#include "../core/models.h"
#include "threads.h"

struct vita2d_texture;

namespace vt {

class Player {
public:
    enum class State { Loading, Buffering, Playing, Paused, Ended, Error };

    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Main thread. durationHint is used until the segment index is loaded.
    bool start(const Format& video, const Format& audio, const std::string& userAgent, double durationHint,
               double startAt);
    void stop();

    void setPaused(bool paused);
    bool paused() const { return paused_; }
    void seek(double seconds);
    // Playback speed, 0.25 to 2. Audio is time-stretched so voices keep their pitch.
    void setSpeed(double speed);
    double speed() const;
    double position() const;
    double duration() const;
    double bufferedAhead() const;  // seconds of video downloaded past the playhead
    State state() const;
    std::string error() const;
    int videoHeight() const { return video_.fmt.height; }

    // Main thread: chooses the frame for the current clock and draws it letterboxed.
    void draw(float x, float y, float w, float h);

private:
    struct Segment {
        uint32_t gen = 0;
        double start = 0, end = 0;
        std::vector<uint8_t> data;
        std::vector<Sample> samples;
    };
    struct Track {
        Format fmt;
        Fmp4Init init;
        SegmentIndex index;
        std::deque<std::shared_ptr<Segment>> queue;
        size_t queuedBytes = 0;
        size_t next = 0;
        bool eos = false;
        double bufferedEnd = 0;
    };
    enum SlotState { FREE, DECODING, READY, SHOWN, RETIRED };
    struct Slot {
        vita2d_texture* tex = nullptr;
        void* data = nullptr;
        SlotState state = FREE;
        double pts = 0;
        uint32_t gen = 0;
        uint32_t retiredAt = 0;
    };

    void loaderMain();
    void videoMain();
    void audioMain();
    bool loadInit(Track& t);
    Track* pickTrackLocked();
    void applySeekLocked(double t);
    double positionLocked() const;
    void fail(const std::string& msg);
    int acquireSlot(uint32_t gen);
    void finishSlot(int slot, bool keep, double pts, uint32_t gen);

    mutable std::mutex m_;
    std::condition_variable cv_;
    Track video_, audio_;
    std::string ua_;
    std::atomic<uint32_t> gen_{1};
    double genStart_ = 0, genTarget_ = 0;
    double seekWall_ = 0;  // when the last seek was applied (for the log)
    double startAt_ = 0, duration_ = 0;
    std::atomic<bool> quit_{false}, cancelDl_{false}, paused_{false}, ended_{false};
    bool initDone_ = false;
    std::string error_;

    // Clock, guarded by m_.
    double clockPts_ = 0, clockWall_ = 0, audioFrameDur_ = 1024.0 / 44100.0;
    double speed_ = 1.0;
    bool clockRunning_ = false, waitingData_ = true, noAudio_ = false;
    bool videoEos_ = false, audioEos_ = false;

    std::mutex fm_;
    std::condition_variable fcv_;
    std::vector<Slot> slots_;
    uint32_t frameNo_ = 0;
    int shown_ = -1;
    int texW_ = 0, texH_ = 0, picW_ = 0, picH_ = 0;

    Thread loader_, vthread_, athread_;
    bool started_ = false;
};

}  // namespace vt
