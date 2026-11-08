#include "emoji.h"

#include <cstdio>
#include <cstring>

#include "util.h"

namespace vt {

namespace {

constexpr uint32_t kZwj = 0x200D, kVs16 = 0xFE0F, kKeycap = 0x20E3;

bool is_tone(uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
bool is_regional(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool is_tag(uint32_t cp) { return cp >= 0xE0020 && cp <= 0xE007F; }

// Characters below U+1F000 that show as emoji even without U+FE0F (Emoji_Presentation=Yes).
bool emoji_presentation(uint32_t cp) {
    static const uint32_t ranges[][2] = {
        {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
        {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
        {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
        {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}};
    for (auto& r : ranges)
        if (cp >= r[0] && cp <= r[1]) return true;
    return false;
}

uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

}  // namespace

bool is_emoji_component(uint32_t cp) {
    return cp == kZwj || cp == kKeycap || (cp >= 0xFE00 && cp <= 0xFE0F) || is_tone(cp) || is_tag(cp);
}

bool is_pictograph(uint32_t cp) { return cp >= 0x1F000 && cp <= 0x1FAFF; }

bool EmojiPack::open(const std::string& path, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    // The index is small; read up to where the first image starts.
    std::string head(16, '\0');
    bool ok = size >= 16 && fread(&head[0], 1, 16, f) == 16;
    if (ok) {
        uint32_t count = rd32((const unsigned char*)head.data() + 8);
        size_t indexMax = 16 + (size_t)count * (1 + 4 * 16 + 8);
        if (indexMax > (size_t)size) indexMax = (size_t)size;
        head.resize(indexMax);
        ok = fread(&head[16], 1, indexMax - 16, f) == indexMax - 16;
    }
    fclose(f);
    if (!ok) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    path_ = path;
    return parse(head, (size_t)size, err);
}

bool EmojiPack::openMemory(const std::string& bytes, std::string* err) {
    mem_ = bytes;
    return parse(mem_, mem_.size(), err);
}

bool EmojiPack::parse(const std::string& head, size_t fileSize, std::string* err) {
    map_.clear();
    starts_.clear();
    entries_.clear();
    const unsigned char* p = (const unsigned char*)head.data();
    if (head.size() < 16 || memcmp(p, "VTEMOJI1", 8) != 0) {
        if (err) *err = "not an emoji pack";
        return false;
    }
    uint32_t count = rd32(p + 8);
    maxLen_ = rd32(p + 12);
    size_t pos = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 1 > head.size()) break;
        size_t len = p[pos++];
        if (len == 0 || pos + len * 4 + 8 > head.size()) break;
        std::u32string key;
        for (size_t k = 0; k < len; k++, pos += 4) key.push_back((char32_t)rd32(p + pos));
        Entry e;
        e.offset = rd32(p + pos);
        e.size = rd32(p + pos + 4);
        pos += 8;
        if ((size_t)e.offset + e.size > fileSize) continue;
        starts_.insert((uint32_t)key[0]);
        map_[key] = (int)entries_.size();
        entries_.push_back(e);
    }
    if (entries_.size() != count) {
        if (err) *err = str_printf("emoji pack index is damaged (%u of %u entries)", (unsigned)entries_.size(), count);
        return !entries_.empty();
    }
    return true;
}

int EmojiPack::match(const std::string& s, size_t i, size_t* end, bool* textStyle) const {
    if (i >= s.size() || entries_.empty()) return -1;
    uint32_t first = utf8_decode(s, i);
    if (!starts_.count(first)) return -1;
    // Collect the sequence: code points joined by ZWJ, a keycap mark, tag characters, or a second
    // regional indicator. U+FE0F and skin tones are skipped (pack keys have neither).
    std::u32string seq;
    std::vector<size_t> ends;  // byte offset after each collected code point and what follows it
    bool vs16 = false, joinNext = false;
    size_t j = i;
    while (j < s.size() && seq.size() < maxLen_) {
        uint32_t cp = utf8_decode(s, j);
        bool take = seq.empty() || joinNext || cp == kZwj || cp == kKeycap || is_tag(cp) ||
                    (seq.size() == 1 && is_regional(seq[0]) && is_regional(cp));
        if (!take) break;
        joinNext = cp == kZwj;
        seq.push_back((char32_t)cp);
        j = utf8_next(s, j);
        while (j < s.size()) {
            uint32_t m = utf8_decode(s, j);
            if (m == kVs16) {
                if (seq.size() == 1) vs16 = true;
            } else if (!is_tone(m)) {
                break;
            }
            j = utf8_next(s, j);
        }
        ends.push_back(j);
    }
    for (size_t len = seq.size(); len > 0; len--) {
        auto it = map_.find(seq.substr(0, len));
        if (it == map_.end()) continue;
        *end = ends[len - 1];
        if (textStyle) *textStyle = len == 1 && first < 0x1F000 && !vs16 && !emoji_presentation(first);
        return it->second;
    }
    return -1;
}

bool EmojiPack::png(int index, std::string& out) const {
    if (index < 0 || index >= (int)entries_.size()) return false;
    const Entry& e = entries_[index];
    if (!mem_.empty()) {
        out.assign(mem_, e.offset, e.size);
        return true;
    }
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return false;
    out.resize(e.size);
    bool ok = fseek(f, (long)e.offset, SEEK_SET) == 0 && fread(&out[0], 1, e.size, f) == e.size;
    fclose(f);
    return ok;
}

}  // namespace vt
