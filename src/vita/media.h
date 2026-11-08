// Thin wrappers over the Vita's public hardware decoders: SceAvcdec (H.264) and SceAudiodec
// (AAC), plus an SceAudioOut port. These are the APIs Vita3K emulates as well.
#pragma once

#include <psp2/audiodec.h>
#include <psp2/types.h>
#include <psp2/videodec.h>

#include <cstddef>
#include <cstdint>

#include "../core/fmp4.h"

namespace vt {

// H.264 decoder that writes YUV420 planar frames (Y plane `pitch` x height, then U and V at
// pitch/2) into caller-supplied buffers, matching SCE_GXM_TEXTURE_FORMAT_YUV420P3 textures.
class AvcDecoder {
public:
    ~AvcDecoder() { close(); }
    bool open(int pitch, int alignedHeight);
    void close();
    bool isOpen() const { return open_; }
    // Decodes one Annex-B access unit. If a picture comes out it is written to `out` and the
    // function returns 1 with *outPts set; 0 if nothing came out yet; <0 on error.
    int decode(const uint8_t* au, size_t size, int64_t pts, int64_t dts, void* out, int64_t* outPts);
    int pitch() const { return pitch_; }
    int height() const { return height_; }

private:
    bool open_ = false;
    bool libInit_ = false;
    int pitch_ = 0, height_ = 0;
    SceAvcdecCtrl ctrl_{};
    SceUID memblock_ = -1;
};

class AacDecoder {
public:
    ~AacDecoder() { close(); }
    bool open(const Fmp4Init& init);
    void close();
    // Decodes one raw AAC frame (an ADTS header is added internally) into interleaved S16
    // stereo. Returns samples per channel, or <0 on error.
    int decode(const uint8_t* frame, size_t size, int16_t* pcm);
    int sampleRate() const { return outRate_; }
    int samplesPerFrame() const { return samples_; }

private:
    bool open_ = false;
    bool libInit_ = false;
    Fmp4Init init_;
    uint8_t* es_ = nullptr;
    size_t esCap_ = 0;
    int16_t* pcm_ = nullptr;
    SceAudiodecCtrl ctrl_{};
    SceAudiodecInfo info_{};
    int outRate_ = 44100, samples_ = 1024;
};

class AudioPort {
public:
    ~AudioPort() { close(); }
    bool open(int sampleRate, int grain);
    void close();
    // Blocks until the previous buffer has been consumed, which paces playback.
    void output(const int16_t* stereo);
    int grain() const { return grain_; }
    int rate() const { return rate_; }

private:
    int port_ = -1;
    int grain_ = 0, rate_ = 0;
};

}  // namespace vt
