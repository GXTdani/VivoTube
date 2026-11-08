// Minimal fragmented-MP4 (DASH) demuxer for YouTube's adaptive H.264 / AAC streams.
//
// A YouTube DASH stream is: ftyp + moov (codec config) + sidx (segment index), followed by
// moof+mdat fragment pairs. The player downloads [0, indexRange.end] first, parses it with
// fmp4_parse_init, then fetches fragments by byte range and parses each with
// fmp4_parse_fragments.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vt {

struct Fmp4Init {
    uint32_t trackId = 0;
    uint32_t timescale = 0;
    int64_t editShift = 0;  // media time to subtract (edit list), in timescale units

    bool isVideo = false;
    int width = 0, height = 0;
    int nalLengthSize = 4;
    std::vector<std::vector<uint8_t>> sps, pps;

    bool isAudio = false;
    std::vector<uint8_t> asc;  // AudioSpecificConfig
    int audioObjectType = 0, freqIndex = 0, sampleRate = 0, channels = 0;
    bool sbr = false;

    // trex defaults
    uint32_t defDuration = 0, defSize = 0, defFlags = 0;
};

struct SegmentRef {
    uint64_t offset = 0;   // absolute byte offset in the file
    uint32_t size = 0;
    uint64_t start = 0;    // presentation start, sidx timescale
    uint32_t duration = 0;
};

struct SegmentIndex {
    uint32_t timescale = 0;
    std::vector<SegmentRef> refs;
    double startSeconds(size_t i) const { return timescale ? (double)refs[i].start / timescale : 0; }
    double durationSeconds(size_t i) const { return timescale ? (double)refs[i].duration / timescale : 0; }
    // Index of the segment containing time t (clamped).
    size_t find(double t) const;
};

struct Sample {
    int64_t dts = 0, pts = 0;  // track timescale
    uint32_t duration = 0;
    uint32_t offset = 0, size = 0;  // within the parsed buffer
    bool key = false;
};

// Parses ftyp/moov and, if present in the same buffer, the sidx. bufferFileOffset is the file
// offset of data[0] (normally 0).
bool fmp4_parse_init(const uint8_t* data, size_t size, Fmp4Init& init, SegmentIndex* index,
                     std::string* err = nullptr, uint64_t bufferFileOffset = 0);

// Parses all complete moof+mdat pairs in the buffer, appending samples in decode order.
bool fmp4_parse_fragments(const uint8_t* data, size_t size, const Fmp4Init& init, std::vector<Sample>& out,
                          std::string* err = nullptr);

// Builds the 7-byte ADTS header for one raw AAC frame of payloadSize bytes.
void make_adts_header(const Fmp4Init& init, size_t payloadSize, uint8_t out[7]);

// Converts length-prefixed NAL units to Annex-B (00 00 00 01 start codes). When withParams is
// set, SPS and PPS from the init segment are emitted first. Returns false on malformed input.
bool avcc_to_annexb(const Fmp4Init& init, const uint8_t* data, size_t size, bool withParams,
                    std::vector<uint8_t>& out);

}  // namespace vt
