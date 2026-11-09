// Time stretching for playback speed: plays audio faster or slower without changing its pitch.
// The audio is cut into overlapping pieces that are spliced back together where their waveforms
// line up best (WSOLA, the same idea as SoundTouch's TDStretch). Interleaved 16-bit stereo.
// Platform-neutral.
#pragma once

#include <cstdint>
#include <vector>

namespace vt {

class TimeStretch {
public:
    void setSampleRate(int rate);  // sets the piece, overlap and search lengths
    void setSpeed(double speed);   // 0.25 .. 2
    double speed() const { return speed_; }
    void clear();                  // drops everything buffered (after a seek)

    void put(const int16_t* pcm, int frames);
    int available() const { return (int)(out_.size() / 2 - outRead_); }
    // Copies up to `frames` stretched frames to out; returns how many.
    int take(int16_t* out, int frames);
    // The input position (in frames since clear()) of the next frame take() returns, so the
    // player's clock can follow the media time of what is playing.
    double position() const { return outPos_; }

private:
    void process();
    int bestOffset(int at) const;

    int seq_ = 1764, overlap_ = 353, seek_ = 662;  // frames, for 44.1 kHz
    double speed_ = 1.0;
    std::vector<int16_t> in_;   // interleaved input not yet consumed
    double inBase_ = 0;         // input frames dropped from the front of in_ since clear()
    double pos_ = 0;            // next piece's nominal start, in frames into in_
    std::vector<float> mid_;    // the end of the previous piece, faded into the next one
    bool haveMid_ = false;
    std::vector<int16_t> out_;  // stretched output, read from outRead_ (frames)
    size_t outRead_ = 0;
    double outPos_ = 0;
};

}  // namespace vt
