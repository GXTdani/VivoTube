#include "stretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vt {

void TimeStretch::setSampleRate(int rate) {
    if (rate <= 0) rate = 44100;
    seq_ = rate * 40 / 1000;      // 40 ms pieces
    overlap_ = rate * 8 / 1000;   // faded over 8 ms
    seek_ = rate * 15 / 1000;     // searched over 15 ms for the best splice
    clear();
}

void TimeStretch::setSpeed(double speed) { speed_ = std::max(0.25, std::min(2.0, speed)); }

void TimeStretch::clear() {
    in_.clear();
    inBase_ = 0;
    pos_ = 0;
    mid_.assign((size_t)overlap_ * 2, 0.0f);
    haveMid_ = false;
    out_.clear();
    outRead_ = 0;
    outPos_ = 0;
}

void TimeStretch::put(const int16_t* pcm, int frames) {
    if (frames <= 0) return;
    in_.insert(in_.end(), pcm, pcm + (size_t)frames * 2);
    process();
}

int TimeStretch::take(int16_t* out, int frames) {
    int n = std::min(frames, available());
    if (n <= 0) return 0;
    memcpy(out, out_.data() + outRead_ * 2, (size_t)n * 4);
    outRead_ += n;
    outPos_ += n * speed_;
    if (outRead_ * 2 >= out_.size()) {
        out_.clear();
        outRead_ = 0;
    } else if (outRead_ > 16384) {
        out_.erase(out_.begin(), out_.begin() + outRead_ * 2);
        outRead_ = 0;
    }
    return n;
}

// The offset in [0, seek_) where the input at `at` lines up best with the faded-out end of
// the previous piece: normalised cross-correlation of the channel sums, searched coarsely and
// then refined.
int TimeStretch::bestOffset(int at) const {
    auto corr = [&](int k, int stride) {
        double num = 0, energy = 1e-9;
        const int16_t* p = in_.data() + (size_t)(at + k) * 2;
        for (int i = 0; i < overlap_; i += stride) {
            float b = (float)p[i * 2] + (float)p[i * 2 + 1];
            float a = mid_[(size_t)i * 2] + mid_[(size_t)i * 2 + 1];
            num += a * b;
            energy += b * b;
        }
        return num / std::sqrt(energy);
    };
    int best = 0;
    double bestC = -1e300;
    for (int k = 0; k < seek_; k += 4) {
        double c = corr(k, 4);
        if (c > bestC) bestC = c, best = k;
    }
    int lo = std::max(0, best - 3), hi = std::min(seek_ - 1, best + 3);
    bestC = -1e300;
    int refined = best;
    for (int k = lo; k <= hi; k++) {
        double c = corr(k, 1);
        if (c > bestC) bestC = c, refined = k;
    }
    return refined;
}

void TimeStretch::process() {
    const int stride = seq_ - overlap_;  // frames output per piece
    for (;;) {
        int start = (int)pos_;
        int frames = (int)(in_.size() / 2);
        if (start + seek_ + seq_ > frames) break;
        int s = start + (haveMid_ ? bestOffset(start) : 0);
        const int16_t* src = in_.data() + (size_t)s * 2;
        if (available() == 0) outPos_ = inBase_ + s;
        size_t o = out_.size();
        out_.resize(o + (size_t)stride * 2);
        int16_t* dst = out_.data() + o;
        for (int i = 0; i < overlap_; i++) {
            float w = haveMid_ ? (float)(i + 1) / (overlap_ + 1) : 1.0f;
            for (int c = 0; c < 2; c++) {
                float v = mid_[(size_t)i * 2 + c] * (1.0f - w) + src[i * 2 + c] * w;
                dst[i * 2 + c] = (int16_t)std::max(-32768.0f, std::min(32767.0f, v));
            }
        }
        memcpy(dst + (size_t)overlap_ * 2, src + (size_t)overlap_ * 2, (size_t)(seq_ - 2 * overlap_) * 4);
        for (int i = 0; i < overlap_ * 2; i++) mid_[(size_t)i] = src[(size_t)(seq_ - overlap_) * 2 + i];
        haveMid_ = true;
        pos_ += stride * speed_;
        // Forget input the next pieces can no longer reach. Near 2x the next piece can start
        // beyond the input buffered so far.
        int drop = std::min((int)pos_, (int)(in_.size() / 2));
        if (drop > 4096) {
            in_.erase(in_.begin(), in_.begin() + (size_t)drop * 2);
            pos_ -= drop;
            inBase_ += drop;
        }
    }
}

}  // namespace vt
