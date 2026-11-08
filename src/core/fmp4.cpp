#include "fmp4.h"

#include <cstring>

namespace vt {

namespace {

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool bad = false;

    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    bool has(size_t k) const { return !bad && pos + k <= n; }
    void skip(size_t k) {
        if (!has(k)) bad = true;
        else pos += k;
    }
    uint8_t u8() {
        if (!has(1)) {
            bad = true;
            return 0;
        }
        return p[pos++];
    }
    uint16_t u16() {
        uint16_t a = u8();
        return (uint16_t)((a << 8) | u8());
    }
    uint32_t u24() {
        uint32_t a = u16();
        return (a << 8) | u8();
    }
    uint32_t u32() {
        uint32_t a = u16();
        return (a << 16) | u16();
    }
    uint64_t u64() {
        uint64_t a = u32();
        return (a << 32) | u32();
    }
};

struct Box {
    uint32_t type = 0;
    size_t start = 0;    // offset of box header in the parent buffer
    size_t body = 0;     // offset of payload
    size_t end = 0;      // offset one past the box
};

constexpr uint32_t fourcc(const char* s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) |
           (uint32_t)(uint8_t)s[3];
}

// Reads the box header at r.pos within [.., limit). Returns false if incomplete.
bool next_box(Reader& r, size_t limit, Box& b) {
    if (r.pos + 8 > limit) return false;
    b.start = r.pos;
    uint64_t size = r.u32();
    b.type = r.u32();
    if (size == 1) {
        if (r.pos + 8 > limit) return false;
        size = r.u64();
    } else if (size == 0) {
        size = limit - b.start;
    }
    b.body = r.pos;
    if (size < (uint64_t)(b.body - b.start) || b.start + size > limit) return false;
    b.end = b.start + (size_t)size;
    return true;
}

const int kAacRates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                           16000, 12000, 11025, 8000,  7350,  0,     0,     0};

struct BitReader {
    const uint8_t* p;
    size_t n, bit = 0;
    uint32_t read(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; i++) {
            size_t byte = bit >> 3;
            uint32_t b = byte < n ? (p[byte] >> (7 - (bit & 7))) & 1 : 0;
            v = (v << 1) | b;
            bit++;
        }
        return v;
    }
};

void parse_asc(Fmp4Init& init) {
    if (init.asc.size() < 2) return;
    BitReader br{init.asc.data(), init.asc.size()};
    int aot = (int)br.read(5);
    if (aot == 31) aot = 32 + (int)br.read(6);
    int fi = (int)br.read(4);
    int rate = fi == 15 ? (int)br.read(24) : kAacRates[fi];
    int ch = (int)br.read(4);
    if (aot == 5 || aot == 29) {  // explicit SBR signalling: core config follows
        init.sbr = true;
        int efi = (int)br.read(4);
        if (efi == 15) br.read(24);
        aot = (int)br.read(5);
        if (aot == 31) aot = 32 + (int)br.read(6);
    }
    init.audioObjectType = aot;
    init.freqIndex = fi;
    init.sampleRate = rate;
    init.channels = ch;
}

uint32_t read_desc_len(Reader& r) {
    uint32_t len = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = r.u8();
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

void parse_esds(Reader& r, size_t end, Fmp4Init& init) {
    r.skip(4);  // version + flags
    while (!r.bad && r.pos + 2 <= end) {
        uint8_t tag = r.u8();
        uint32_t len = read_desc_len(r);
        size_t dend = r.pos + len;
        if (dend > end) return;
        if (tag == 0x03) {
            r.skip(2);
            uint8_t flags = r.u8();
            if (flags & 0x80) r.skip(2);
            if (flags & 0x40) r.skip(r.u8());
            if (flags & 0x20) r.skip(2);
            continue;  // descend into nested descriptors
        }
        if (tag == 0x04) {
            r.skip(13);
            continue;
        }
        if (tag == 0x05) {
            init.asc.assign(r.p + r.pos, r.p + dend);
            parse_asc(init);
            return;
        }
        r.pos = dend;
    }
}

void parse_avcc(Reader& r, size_t end, Fmp4Init& init) {
    r.skip(4);  // version, profile, compat, level
    init.nalLengthSize = (r.u8() & 3) + 1;
    int nsps = r.u8() & 0x1F;
    for (int i = 0; i < nsps && !r.bad; i++) {
        uint16_t len = r.u16();
        if (r.pos + len > end) return;
        init.sps.emplace_back(r.p + r.pos, r.p + r.pos + len);
        r.skip(len);
    }
    int npps = r.u8();
    for (int i = 0; i < npps && !r.bad; i++) {
        uint16_t len = r.u16();
        if (r.pos + len > end) return;
        init.pps.emplace_back(r.p + r.pos, r.p + r.pos + len);
        r.skip(len);
    }
}

void parse_stsd(Reader& r, size_t end, Fmp4Init& init) {
    r.skip(4);
    uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && !r.bad; i++) {
        Box e;
        if (!next_box(r, end, e)) return;
        if (e.type == fourcc("avc1") || e.type == fourcc("avc3")) {
            init.isVideo = true;
            r.pos = e.body + 24;
            init.width = r.u16();
            init.height = r.u16();
            r.pos = e.body + 78;
            Box c;
            while (next_box(r, e.end, c)) {
                if (c.type == fourcc("avcC")) parse_avcc(r, c.end, init);
                r.pos = c.end;
            }
        } else if (e.type == fourcc("mp4a")) {
            init.isAudio = true;
            r.pos = e.body + 16;
            init.channels = r.u16();
            r.skip(6);
            init.sampleRate = (int)(r.u32() >> 16);
            r.pos = e.body + 28;
            Box c;
            while (next_box(r, e.end, c)) {
                if (c.type == fourcc("esds")) parse_esds(r, c.end, init);
                r.pos = c.end;
            }
        }
        r.pos = e.end;
        return;  // first sample entry only
    }
}

void parse_container(Reader& r, size_t end, Fmp4Init& init) {
    Box b;
    while (!r.bad && next_box(r, end, b)) {
        switch (b.type) {
            case fourcc("trak"):
            case fourcc("mdia"):
            case fourcc("minf"):
            case fourcc("stbl"):
            case fourcc("mvex"):
            case fourcc("edts"):
                parse_container(r, b.end, init);
                break;
            case fourcc("tkhd"): {
                uint8_t ver = r.u8();
                r.skip(3);
                r.skip(ver == 1 ? 16 : 8);
                init.trackId = r.u32();
                break;
            }
            case fourcc("mdhd"): {
                uint8_t ver = r.u8();
                r.skip(3);
                r.skip(ver == 1 ? 16 : 8);
                init.timescale = r.u32();
                break;
            }
            case fourcc("elst"): {
                uint8_t ver = r.u8();
                r.skip(3);
                uint32_t n = r.u32();
                for (uint32_t i = 0; i < n && !r.bad; i++) {
                    int64_t mediaTime;
                    if (ver == 1) {
                        r.skip(8);
                        mediaTime = (int64_t)r.u64();
                    } else {
                        r.skip(4);
                        mediaTime = (int32_t)r.u32();
                    }
                    r.skip(4);
                    if (mediaTime >= 0) {
                        init.editShift = mediaTime;
                        break;
                    }
                }
                break;
            }
            case fourcc("stsd"):
                parse_stsd(r, b.end, init);
                break;
            case fourcc("trex"):
                r.skip(4 + 4 + 4);  // version/flags, track_ID, default_sample_description_index
                init.defDuration = r.u32();
                init.defSize = r.u32();
                init.defFlags = r.u32();
                break;
            default:
                break;
        }
        r.pos = b.end;
    }
}

void parse_sidx(Reader& r, const Box& b, uint64_t fileBase, SegmentIndex& idx) {
    uint8_t ver = r.u8();
    r.skip(3);
    r.skip(4);  // reference_ID
    idx.timescale = r.u32();
    uint64_t ept, firstOffset;
    if (ver == 0) {
        ept = r.u32();
        firstOffset = r.u32();
    } else {
        ept = r.u64();
        firstOffset = r.u64();
    }
    r.skip(2);
    uint16_t count = r.u16();
    uint64_t offset = fileBase + b.end + firstOffset;
    uint64_t t = ept;
    idx.refs.clear();
    for (uint16_t i = 0; i < count && !r.bad; i++) {
        uint32_t a = r.u32();
        uint32_t dur = r.u32();
        r.skip(4);
        SegmentRef s;
        s.offset = offset;
        s.size = a & 0x7FFFFFFF;
        s.start = t;
        s.duration = dur;
        idx.refs.push_back(s);
        offset += s.size;
        t += dur;
    }
}

}  // namespace

size_t SegmentIndex::find(double t) const {
    if (refs.empty()) return 0;
    for (size_t i = 0; i < refs.size(); i++) {
        if (startSeconds(i) + durationSeconds(i) > t) return i;
    }
    return refs.size() - 1;
}

bool fmp4_parse_init(const uint8_t* data, size_t size, Fmp4Init& init, SegmentIndex* index, std::string* err,
                     uint64_t bufferFileOffset) {
    Reader r(data, size);
    Box b;
    bool haveMoov = false;
    while (next_box(r, size, b)) {
        if (b.type == fourcc("moov")) {
            parse_container(r, b.end, init);
            haveMoov = true;
        } else if (b.type == fourcc("sidx") && index) {
            parse_sidx(r, b, bufferFileOffset, *index);
        }
        r.pos = b.end;
    }
    if (!haveMoov || r.bad) {
        if (err) *err = "init segment has no moov box";
        return false;
    }
    if (!init.timescale) {
        if (err) *err = "missing media timescale";
        return false;
    }
    if (init.isVideo && (init.sps.empty() || init.pps.empty())) {
        if (err) *err = "missing SPS/PPS";
        return false;
    }
    if (init.isAudio && init.asc.empty()) {
        if (err) *err = "missing AudioSpecificConfig";
        return false;
    }
    if (index && index->refs.empty()) {
        if (err) *err = "missing segment index (sidx)";
        return false;
    }
    return true;
}

bool fmp4_parse_fragments(const uint8_t* data, size_t size, const Fmp4Init& init, std::vector<Sample>& out,
                          std::string* err) {
    Reader r(data, size);
    Box moof;
    bool any = false;
    while (next_box(r, size, moof)) {
        if (moof.type != fourcc("moof")) {
            r.pos = moof.end;
            continue;
        }
        Box traf;
        Reader tr(data, moof.end);
        tr.pos = moof.body;
        while (next_box(tr, moof.end, traf)) {
            if (traf.type != fourcc("traf")) {
                tr.pos = traf.end;
                continue;
            }
            uint32_t tfFlags = 0, defDur = init.defDuration, defSize = init.defSize, defFlags = init.defFlags;
            uint64_t base = moof.start;
            int64_t decodeTime = 0;
            uint64_t nextData = 0;
            bool haveNext = false;
            Box c;
            Reader cr(data, traf.end);
            cr.pos = traf.body;
            while (next_box(cr, traf.end, c)) {
                if (c.type == fourcc("tfhd")) {
                    cr.u8();
                    tfFlags = cr.u24();
                    cr.skip(4);
                    if (tfFlags & 0x1) base = cr.u64();  // absolute; assumes buffer starts at file offset 0
                    if (tfFlags & 0x2) cr.skip(4);
                    if (tfFlags & 0x8) defDur = cr.u32();
                    if (tfFlags & 0x10) defSize = cr.u32();
                    if (tfFlags & 0x20) defFlags = cr.u32();
                } else if (c.type == fourcc("tfdt")) {
                    uint8_t ver = cr.u8();
                    cr.skip(3);
                    decodeTime = ver == 1 ? (int64_t)cr.u64() : (int64_t)cr.u32();
                } else if (c.type == fourcc("trun")) {
                    uint8_t ver = cr.u8();
                    uint32_t flags = cr.u24();
                    uint32_t count = cr.u32();
                    uint64_t pos = haveNext ? nextData : base;
                    if (flags & 0x1) pos = base + (int64_t)(int32_t)cr.u32();
                    uint32_t firstFlags = 0;
                    bool haveFirst = (flags & 0x4) != 0;
                    if (haveFirst) firstFlags = cr.u32();
                    for (uint32_t i = 0; i < count && !cr.bad; i++) {
                        Sample s;
                        s.duration = (flags & 0x100) ? cr.u32() : defDur;
                        s.size = (flags & 0x200) ? cr.u32() : defSize;
                        uint32_t sf = (flags & 0x400) ? cr.u32() : defFlags;
                        if (i == 0 && haveFirst) sf = firstFlags;
                        int64_t cto = 0;
                        if (flags & 0x800) cto = ver == 0 ? (int64_t)cr.u32() : (int64_t)(int32_t)cr.u32();
                        s.dts = decodeTime;
                        s.pts = decodeTime + cto - init.editShift;
                        s.key = init.isAudio || !((sf >> 16) & 1);
                        s.offset = (uint32_t)pos;
                        if (pos + s.size > size) {
                            if (err) *err = "fragment data truncated";
                            return any;
                        }
                        out.push_back(s);
                        any = true;
                        pos += s.size;
                        decodeTime += s.duration;
                    }
                    nextData = pos;
                    haveNext = true;
                }
                cr.pos = c.end;
            }
            tr.pos = traf.end;
        }
        r.pos = moof.end;
    }
    if (!any && err && err->empty()) *err = "no samples in fragment";
    return any;
}

void make_adts_header(const Fmp4Init& init, size_t payloadSize, uint8_t h[7]) {
    size_t len = payloadSize + 7;
    int profile = (init.audioObjectType > 0 ? init.audioObjectType : 2) - 1;
    int fi = init.freqIndex;
    int ch = init.channels > 0 ? init.channels : 2;
    h[0] = 0xFF;
    h[1] = 0xF1;  // MPEG-4, layer 0, no CRC
    h[2] = (uint8_t)(((profile & 3) << 6) | ((fi & 0xF) << 2) | ((ch >> 2) & 1));
    h[3] = (uint8_t)(((ch & 3) << 6) | ((len >> 11) & 3));
    h[4] = (uint8_t)((len >> 3) & 0xFF);
    h[5] = (uint8_t)(((len & 7) << 5) | 0x1F);
    h[6] = 0xFC;
}

bool avcc_to_annexb(const Fmp4Init& init, const uint8_t* data, size_t size, bool withParams,
                    std::vector<uint8_t>& out) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.clear();
    out.reserve(size + 64);
    if (withParams) {
        for (auto& s : init.sps) {
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), s.begin(), s.end());
        }
        for (auto& p : init.pps) {
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), p.begin(), p.end());
        }
    }
    size_t pos = 0;
    const int ls = init.nalLengthSize;
    while (pos + ls <= size) {
        uint32_t len = 0;
        for (int i = 0; i < ls; i++) len = (len << 8) | data[pos + i];
        pos += ls;
        if (len == 0 || pos + len > size) return false;
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), data + pos, data + pos + len);
        pos += len;
    }
    return pos == size;
}

}  // namespace vt
