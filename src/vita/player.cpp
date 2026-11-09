#include "player.h"

#include "../core/stretch.h"

#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../core/http.h"
#include "../core/util.h"
#include "media.h"

namespace vt {

namespace {

const int kSlots = 5;
const double kAheadSeconds = 30.0;
const size_t kMaxQueuedBytes = 24u << 20;
const double kPrebuffer = 2.0;
const int64_t kChunk = 4 << 20;

int align_to(int v, int a) { return (v + a - 1) / a * a; }

void sleep_ms(int ms) { sceKernelDelayThread((SceUInt)ms * 1000); }

}  // namespace

Player::~Player() { stop(); }

bool Player::start(const Format& video, const Format& audio, const std::string& userAgent, double durationHint,
                   double startAt) {
    stop();
    video_ = Track();
    audio_ = Track();
    video_.fmt = video;
    audio_.fmt = audio;
    ua_ = userAgent;
    duration_ = durationHint;
    startAt_ = startAt;
    gen_ = 1;
    quit_ = false;
    cancelDl_ = false;
    paused_ = false;
    ended_ = false;
    initDone_ = false;
    error_.clear();
    clockPts_ = startAt;
    clockRunning_ = false;
    waitingData_ = true;
    noAudio_ = false;
    videoEos_ = audioEos_ = false;
    frameNo_ = 0;
    shown_ = -1;

    picW_ = video.width > 0 ? video.width : 640;
    picH_ = video.height > 0 ? video.height : 360;
    texW_ = align_to(picW_, 32);
    texH_ = align_to(picH_, 16);
    slots_.assign(kSlots, Slot());
    for (Slot& s : slots_) {
        s.tex = vita2d_create_empty_texture_format(texW_, texH_, SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC1);
        if (!s.tex) {
            VT_LOG("player: cannot allocate %dx%d frame texture", texW_, texH_);
            for (Slot& f : slots_)
                if (f.tex) vita2d_free_texture(f.tex);
            slots_.clear();
            error_ = "Not enough video memory for this quality";
            return false;
        }
        s.data = vita2d_texture_get_datap(s.tex);
        // Black in YUV: Y=16, U=V=128.
        memset(s.data, 16, (size_t)texW_ * texH_);
        memset((uint8_t*)s.data + (size_t)texW_ * texH_, 128, (size_t)texW_ * texH_ / 2);
        vita2d_texture_set_filters(s.tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    VT_LOG("player: itag %d %dx%d + itag %d, start %.1fs", video.itag, video.width, video.height, audio.itag, startAt);
    started_ = true;
    loader_.start([this] { loaderMain(); }, 256 * 1024);
    vthread_.start([this] { videoMain(); }, 256 * 1024);
    athread_.start([this] { audioMain(); }, 128 * 1024);
    return true;
}

void Player::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        quit_ = true;
        cancelDl_ = true;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(fm_);
    }
    fcv_.notify_all();
    loader_.join();
    vthread_.join();
    athread_.join();
    vita2d_wait_rendering_done();
    for (Slot& s : slots_)
        if (s.tex) vita2d_free_texture(s.tex);
    slots_.clear();
    started_ = false;
}

void Player::fail(const std::string& msg) {
    VT_LOG("player: error: %s", msg.c_str());
    std::lock_guard<std::mutex> lock(m_);
    if (error_.empty()) error_ = msg;
}

std::string Player::error() const {
    std::lock_guard<std::mutex> lock(m_);
    return error_;
}

double Player::positionLocked() const {
    if (!clockRunning_) return clockPts_;
    double dt = (monotonic_seconds() - clockWall_) * speed_;
    if (noAudio_) return clockPts_ + dt;
    return clockPts_ + std::min(dt, audioFrameDur_ * 1.5 * speed_);
}

void Player::setSpeed(double speed) {
    speed = std::max(0.25, std::min(2.0, speed));
    std::lock_guard<std::mutex> lock(m_);
    if (speed == speed_) return;
    clockPts_ = positionLocked();  // the clock carries on from here at the new rate
    clockWall_ = monotonic_seconds();
    speed_ = speed;
    VT_LOG("player: speed %.2fx", speed);
}

double Player::speed() const {
    std::lock_guard<std::mutex> lock(m_);
    return speed_;
}

double Player::position() const {
    std::lock_guard<std::mutex> lock(m_);
    return positionLocked();
}

double Player::duration() const {
    std::lock_guard<std::mutex> lock(m_);
    return duration_;
}

double Player::bufferedAhead() const {
    std::lock_guard<std::mutex> lock(m_);
    double ahead = video_.bufferedEnd - positionLocked();
    return ahead > 0 ? ahead : 0;
}

Player::State Player::state() const {
    std::lock_guard<std::mutex> lock(m_);
    if (!error_.empty()) return State::Error;
    if (ended_) return State::Ended;
    if (!initDone_) return State::Loading;
    if (paused_) return State::Paused;
    if (waitingData_) return State::Buffering;
    return State::Playing;
}

void Player::setPaused(bool p) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (p == paused_) return;
        if (p) {
            clockPts_ = positionLocked();
            clockRunning_ = false;
        } else if (noAudio_) {
            clockWall_ = monotonic_seconds();
            clockRunning_ = true;
        }
        paused_ = p;
    }
    cv_.notify_all();
}

void Player::applySeekLocked(double t) {
    double dur = duration_ > 1 ? duration_ : 1e9;
    if (t > dur - 1.0) t = dur - 1.0;
    if (t < 0) t = 0;
    size_t vi = video_.index.find(t);
    genStart_ = video_.index.startSeconds(vi);
    genTarget_ = t;
    size_t ai = audio_.index.find(genStart_);
    for (Track* tr : {&video_, &audio_}) {
        tr->queue.clear();
        tr->queuedBytes = 0;
        tr->eos = false;
    }
    video_.next = vi;
    audio_.next = ai;
    video_.bufferedEnd = genStart_;
    audio_.bufferedEnd = audio_.index.refs.empty() ? genStart_ : audio_.index.startSeconds(ai);
    VT_LOG("player: seek to %.1fs (video segment %u from %.1fs)", t, (unsigned)vi, genStart_);
    seekWall_ = monotonic_seconds();
    gen_++;
    cancelDl_ = true;
    clockPts_ = t;
    clockWall_ = monotonic_seconds();
    clockRunning_ = false;
    waitingData_ = true;
    videoEos_ = audioEos_ = false;
    ended_ = false;
}

void Player::seek(double t) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!initDone_) {
            startAt_ = t;
            clockPts_ = t;
            return;
        }
        applySeekLocked(t);
    }
    cv_.notify_all();
    fcv_.notify_all();
}

bool Player::loadInit(Track& t) {
    std::string lastErr;
    for (int attempt = 0; attempt < 4 && !quit_; attempt++) {
        if (attempt) sleep_ms(500 << attempt);
        HttpRequest r;
        r.url = t.fmt.url;
        r.userAgent = ua_;
        r.rangeStart = 0;
        r.rangeEnd = t.fmt.indexEnd;
        r.cancel = &quit_;
        HttpResponse res = http_request(r);
        if (!res.ok()) {
            lastErr = res.describe();
            VT_LOG("player: init itag %d attempt %d: %s", t.fmt.itag, attempt, lastErr.c_str());
            if (res.status == 403 || res.status == 404) break;  // retrying will not help
            continue;
        }
        std::string err;
        if (!fmp4_parse_init((const uint8_t*)res.body.data(), res.body.size(), t.init, &t.index, &err)) {
            fail("Unsupported stream format (" + err + ")");
            return false;
        }
        return true;
    }
    if (!quit_)
        fail(lastErr == "HTTP 403" ? "YouTube refused the stream (HTTP 403). The app may need an update."
                                   : "Could not load the video stream (" + lastErr + ")");
    return false;
}

Player::Track* Player::pickTrackLocked() {
    double pos = positionLocked();
    Track* best = nullptr;
    for (Track* t : {&video_, &audio_}) {
        if (t->eos || t->index.refs.empty()) continue;
        if (t->bufferedEnd - pos >= kAheadSeconds || t->queuedBytes >= kMaxQueuedBytes) continue;
        if (!best || t->bufferedEnd < best->bufferedEnd) best = t;
    }
    return best;
}

void Player::loaderMain() {
    if (!loadInit(video_) || !loadInit(audio_)) return;
    if (!video_.init.isVideo || !audio_.init.isAudio) {
        fail("Unexpected stream contents");
        return;
    }
    if (contains(audio_.fmt.codecs, "40.5") || contains(audio_.fmt.codecs, "40.29")) audio_.init.sbr = true;
    double total = 0;
    for (size_t i = 0; i < video_.index.refs.size(); i++) total += video_.index.durationSeconds(i);
    {
        std::lock_guard<std::mutex> lock(m_);
        if (total > 1) duration_ = total;
        initDone_ = true;
        applySeekLocked(startAt_);
        cancelDl_ = false;
    }
    VT_LOG("player: ready, %.1fs, %u video / %u audio segments, video %dx%d", total,
           (unsigned)video_.index.refs.size(), (unsigned)audio_.index.refs.size(), video_.init.width,
           video_.init.height);
    cv_.notify_all();

    while (!quit_) {
        Track* t = nullptr;
        SegmentRef ref;
        uint32_t gen;
        size_t idx;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return quit_ || pickTrackLocked() != nullptr; });
            if (quit_) break;
            t = pickTrackLocked();
            if (!t) continue;
            idx = t->next;
            ref = t->index.refs[idx];
            gen = gen_;
            cancelDl_ = false;
        }
        auto seg = std::make_shared<Segment>();
        seg->gen = gen;
        seg->start = t->index.startSeconds(idx);
        seg->end = seg->start + t->index.durationSeconds(idx);
        seg->data.reserve(ref.size);
        bool ok = true;
        std::string lastErr;
        for (int64_t off = 0; off < (int64_t)ref.size && ok && !quit_ && gen_ == gen;) {
            int64_t len = std::min<int64_t>(kChunk, (int64_t)ref.size - off);
            bool got = false;
            for (int attempt = 0; attempt < 5 && !quit_ && gen_ == gen; attempt++) {
                if (attempt) sleep_ms(std::min(4000, 400 << attempt));
                HttpRequest r;
                r.url = t->fmt.url;
                r.userAgent = ua_;
                r.rangeStart = (int64_t)ref.offset + off;
                r.rangeEnd = (int64_t)ref.offset + off + len - 1;
                r.cancel = &cancelDl_;
                r.maxBytes = (size_t)len + 1024;
                HttpResponse res = http_request(r);
                if (res.ok() && (int64_t)res.body.size() == len) {
                    seg->data.insert(seg->data.end(), res.body.begin(), res.body.end());
                    got = true;
                    break;
                }
                lastErr = res.error == "cancelled" ? res.error : res.describe();
                if (res.error == "cancelled") break;
                VT_LOG("player: segment %u itag %d attempt %d: %s (%u bytes)", (unsigned)idx, t->fmt.itag, attempt,
                       lastErr.c_str(), (unsigned)res.body.size());
                if (res.status == 403) break;
            }
            if (!got) ok = false;
            off += len;
        }
        if (quit_) break;
        if (gen_ != gen) continue;  // seeked while downloading
        if (!ok) {
            fail(lastErr == "HTTP 403" ? "YouTube stopped serving this stream (HTTP 403)."
                                       : "Network error while streaming (" + lastErr + ")");
            break;
        }
        std::string err;
        if (!fmp4_parse_fragments(seg->data.data(), seg->data.size(), t->init, seg->samples, &err) ||
            seg->samples.empty()) {
            fail("Could not read stream data (" + err + ")");
            break;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            if (gen_ != gen) continue;
            t->queuedBytes += seg->data.size();
            t->queue.push_back(seg);
            t->bufferedEnd = seg->end;
            t->next = idx + 1;
            if (t->next >= t->index.refs.size()) t->eos = true;
        }
        cv_.notify_all();
    }
}

int Player::acquireSlot(uint32_t gen) {
    std::unique_lock<std::mutex> lock(fm_);
    while (true) {
        if (quit_ || gen_ != gen) return -1;
        for (int i = 0; i < (int)slots_.size(); i++) {
            if (slots_[i].state == FREE) {
                slots_[i].state = DECODING;
                return i;
            }
        }
        fcv_.wait_for(lock, std::chrono::milliseconds(100));
    }
}

void Player::finishSlot(int slot, bool keep, double pts, uint32_t gen) {
    {
        std::lock_guard<std::mutex> lock(fm_);
        Slot& s = slots_[slot];
        if (keep && gen_ == gen) {
            s.state = READY;
            s.pts = pts;
            s.gen = gen;
        } else {
            s.state = FREE;
        }
    }
    fcv_.notify_all();
}

void Player::videoMain() {
    {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return quit_ || initDone_ || !error_.empty(); });
        if (quit_ || !initDone_) return;
    }
    AvcDecoder dec;
    if (!dec.open(texW_, texH_)) {
        fail("The video decoder could not start");
        return;
    }
    const double ts = video_.init.timescale;
    uint32_t myGen = 0;
    bool needKey = true;
    int errors = 0;
    std::vector<uint8_t> au;
    while (!quit_) {
        std::shared_ptr<Segment> seg;
        uint32_t g;
        double target;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait_for(lock, std::chrono::milliseconds(250),
                         [&] { return quit_ || !video_.queue.empty() || (video_.eos && !videoEos_); });
            if (quit_) break;
            if (video_.queue.empty()) {
                if (video_.eos) videoEos_ = true;
                continue;
            }
            seg = video_.queue.front();
            video_.queue.pop_front();
            video_.queuedBytes -= seg->data.size();
            g = gen_;
            target = genTarget_;
        }
        cv_.notify_all();
        if (seg->gen != g) continue;
        if (myGen != g) {
            // New playback generation (start or seek): restart the decoder so no stale
            // reference frames leak across the jump.
            if (myGen != 0) {
                dec.close();
                if (!dec.open(texW_, texH_)) {
                    fail("The video decoder could not restart");
                    return;
                }
            }
            myGen = g;
            needKey = true;
        }
        for (const Sample& s : seg->samples) {
            if (quit_ || gen_ != g) break;
            if (needKey && !s.key) continue;
            if (!avcc_to_annexb(video_.init, seg->data.data() + s.offset, s.size, needKey || s.key, au)) continue;
            needKey = false;
            int slot = acquireSlot(g);
            if (slot < 0) break;
            int64_t outPts = 0;
            int rc = dec.decode(au.data(), au.size(), s.pts, s.dts, slots_[slot].data, &outPts);
            if (rc < 0) {
                finishSlot(slot, false, 0, g);
                if (++errors == 1 || errors % 50 == 0) VT_LOG("player: decode error 0x%08X (%d)", (unsigned)rc, errors);
                if (errors > 300) {
                    fail("The video decoder keeps failing on this stream");
                    return;
                }
                continue;
            }
            double pts = rc == 1 ? outPts / ts : 0;
            // Frames before a precise seek target are decoded (they are references) but not shown.
            finishSlot(slot, rc == 1 && pts >= target - 0.02, pts, g);
        }
    }
    dec.close();
}

void Player::audioMain() {
    {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return quit_ || initDone_ || !error_.empty(); });
        if (quit_ || !initDone_) return;
    }
    AacDecoder dec;
    AudioPort port;
    bool ok = dec.open(audio_.init) && port.open(dec.sampleRate(), dec.samplesPerFrame());
    {
        std::lock_guard<std::mutex> lock(m_);
        audioFrameDur_ = (double)dec.samplesPerFrame() / (dec.sampleRate() > 0 ? dec.sampleRate() : 44100);
        if (!ok) {
            // Keep playing video on the wall clock rather than failing outright.
            VT_LOG("player: audio unavailable, using wall clock");
            noAudio_ = true;
        }
    }
    std::vector<int16_t> pcm((size_t)dec.samplesPerFrame() * 2 * 2);
    const double ts = audio_.init.timescale;
    const double rate = dec.sampleRate() > 0 ? dec.sampleRate() : 44100;
    // At speeds other than 1 the decoded audio goes through the time stretcher, and the clock
    // follows the media time of the stretched audio being played.
    TimeStretch stretch;
    stretch.setSampleRate((int)rate);
    std::vector<int16_t> grain((size_t)std::max(port.grain(), 1) * 2);
    bool stretching = false;
    uint32_t stretchGen = 0;
    double stretchBase = 0;
    while (!quit_) {
        std::shared_ptr<Segment> seg;
        uint32_t g;
        double target;
        {
            std::unique_lock<std::mutex> lock(m_);
            auto ready = [&] {
                if (quit_) return true;
                if (paused_) return false;
                if (audio_.queue.empty()) return audio_.eos && !audioEos_;
                if (waitingData_) {
                    bool vOk = video_.eos || video_.bufferedEnd - clockPts_ >= kPrebuffer;
                    bool aOk = audio_.eos || audio_.bufferedEnd - clockPts_ >= kPrebuffer;
                    return vOk && aOk;
                }
                return true;
            };
            if (audio_.queue.empty() && !audio_.eos && !waitingData_) {
                // Underrun: freeze the clock until enough data is buffered again.
                clockPts_ = positionLocked();
                clockRunning_ = false;
                waitingData_ = true;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(200), ready);
            if (quit_) break;
            if (!ready()) continue;
            if (audio_.queue.empty()) {
                audioEos_ = true;
                continue;
            }
            if (waitingData_) {
                waitingData_ = false;
                if (seekWall_ > 0) {
                    VT_LOG("player: playing again at %.1fs, %.2fs after the seek", clockPts_,
                           monotonic_seconds() - seekWall_);
                    seekWall_ = 0;
                }
                if (noAudio_) {
                    clockWall_ = monotonic_seconds();
                    clockRunning_ = true;
                }
            }
            seg = audio_.queue.front();
            audio_.queue.pop_front();
            audio_.queuedBytes -= seg->data.size();
            g = gen_;
            target = genTarget_;
        }
        cv_.notify_all();
        if (seg->gen != g || noAudio_) continue;
        for (const Sample& s : seg->samples) {
            if (quit_ || gen_ != g) break;
            double t = s.pts / ts, dur = s.duration / ts;
            if (t + dur <= target) continue;
            if (paused_) {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [&] { return quit_ || !paused_ || gen_ != g; });
                if (quit_ || gen_ != g) break;
            }
            int n = dec.decode(seg->data.data() + s.offset, s.size, pcm.data());
            if (n <= 0) continue;
            double speed;
            {
                std::lock_guard<std::mutex> lock(m_);
                speed = speed_;
            }
            if (speed == 1.0) {
                stretching = false;
                if (n < port.grain()) memset(pcm.data() + n * 2, 0, (size_t)(port.grain() - n) * 4);
                port.output(pcm.data());
                std::lock_guard<std::mutex> lock(m_);
                if (gen_ != g) break;
                clockPts_ = t;
                clockWall_ = monotonic_seconds();
                clockRunning_ = !paused_;
                continue;
            }
            if (!stretching || stretchGen != g) {
                stretch.clear();
                stretching = true;
                stretchGen = g;
                stretchBase = t;
            }
            stretch.setSpeed(speed);
            stretch.put(pcm.data(), n);
            bool stale = false;
            while (!stale && stretch.available() >= port.grain()) {
                double media = stretchBase + stretch.position() / rate;
                stretch.take(grain.data(), port.grain());
                port.output(grain.data());
                std::lock_guard<std::mutex> lock(m_);
                stale = gen_ != g;
                if (stale) break;
                clockPts_ = media;
                clockWall_ = monotonic_seconds();
                clockRunning_ = !paused_;
            }
            if (stale) break;
        }
    }
}

void Player::draw(float x, float y, float w, float h) {
    double pos = position();
    vita2d_texture* tex = nullptr;
    bool anyReady = false;
    {
        std::lock_guard<std::mutex> lock(fm_);
        frameNo_++;
        uint32_t g = gen_;
        for (int i = 0; i < (int)slots_.size(); i++) {
            Slot& s = slots_[i];
            if (s.state == RETIRED && frameNo_ >= s.retiredAt + 2) s.state = FREE;
            if (s.state == READY && s.gen != g) s.state = FREE;
            if (s.state == SHOWN && s.gen != g && i != shown_) {
                s.state = RETIRED;
                s.retiredAt = frameNo_;
            }
        }
        int best = -1;
        for (int i = 0; i < (int)slots_.size(); i++) {
            const Slot& s = slots_[i];
            if (s.state != READY) continue;
            anyReady = true;
            if (s.pts <= pos + 0.008 && (best < 0 || s.pts > slots_[best].pts)) best = i;
        }
        bool staleShown = shown_ >= 0 && slots_[shown_].gen != g;
        if (best < 0 && (shown_ < 0 || staleShown)) {
            // Nothing due yet after a start/seek: preview the earliest decoded frame.
            for (int i = 0; i < (int)slots_.size(); i++)
                if (slots_[i].state == READY && (best < 0 || slots_[i].pts < slots_[best].pts)) best = i;
        }
        if (best >= 0) {
            for (Slot& s : slots_)
                if (s.state == READY && s.pts < slots_[best].pts) s.state = FREE;  // late: drop
            if (shown_ >= 0 && shown_ != best) {
                slots_[shown_].state = RETIRED;
                slots_[shown_].retiredAt = frameNo_;
            }
            slots_[best].state = SHOWN;
            shown_ = best;
        }
        if (shown_ >= 0) tex = slots_[shown_].tex;
    }
    fcv_.notify_all();

    {
        std::lock_guard<std::mutex> lock(m_);
        if (audioEos_ && videoEos_ && !anyReady && video_.queue.empty() && audio_.queue.empty() && initDone_)
            ended_ = true;
    }

    vita2d_draw_rectangle(x, y, w, h, RGBA8(0, 0, 0, 255));
    if (!tex) return;
    float scale = std::min(w / picW_, h / picH_);
    float dw = picW_ * scale, dh = picH_ * scale;
    vita2d_draw_texture_part_scale(tex, x + (w - dw) / 2, y + (h - dh) / 2, 0, 0, (float)picW_, (float)picH_, scale,
                                   scale);
}

}  // namespace vt
