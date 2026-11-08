// Emoji images for the text renderer: the index of assets/emoji/emoji.pak (built by
// tools/make_emoji.py from Twemoji) and the matcher that finds emoji sequences in UTF-8 text.
// Platform-neutral; the textures themselves are made in the UI layer.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vt {

// Code points that only modify or join emoji (variation selectors, ZWJ, skin tones, tags, the
// keycap mark). On their own they draw nothing.
bool is_emoji_component(uint32_t cp);
// Pictographs no font on the system has (used to drop emoji the pack doesn't know).
bool is_pictograph(uint32_t cp);

class EmojiPack {
public:
    // Reads the index from the pack file; images are read from it on demand.
    bool open(const std::string& path, std::string* err);
    // Uses a whole pack held in memory (tests).
    bool openMemory(const std::string& bytes, std::string* err);
    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }

    // Finds the longest known emoji sequence starting at byte offset i of s. Returns its index and
    // sets *end just past it (trailing U+FE0F and skin-tone modifiers included), or returns -1.
    // *textStyle is set for a lone symbol written without U+FE0F whose default look is text
    // (like "©", "❤" or "▶"; not "✨"), which text shows with the font like browsers do.
    int match(const std::string& s, size_t i, size_t* end, bool* textStyle = nullptr) const;
    // The PNG bytes of an entry.
    bool png(int index, std::string& out) const;

private:
    bool parse(const std::string& head, size_t fileSize, std::string* err);
    struct Entry {
        uint32_t offset = 0, size = 0;
    };
    std::unordered_map<std::u32string, int> map_;
    std::unordered_set<uint32_t> starts_;
    std::vector<Entry> entries_;
    size_t maxLen_ = 0;
    std::string path_, mem_;
};

}  // namespace vt
