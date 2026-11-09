#include "text.h"

#include "emoji.h"
#include "util.h"

namespace vt {

#include "text_tables.inc"

namespace {

struct ScriptRange {
    uint32_t first, last;
    Script script;
};

// Sorted by first code point.
const ScriptRange kScripts[] = {
    {0x0530, 0x058F, Script::Armenian},   {0x0590, 0x05FF, Script::Hebrew},     {0x0600, 0x06FF, Script::Arabic},
    {0x0750, 0x077F, Script::Arabic},     {0x0780, 0x07BF, Script::Thaana},     {0x0870, 0x08FF, Script::Arabic},
    {0x0900, 0x097F, Script::Devanagari}, {0x0980, 0x09FF, Script::Bengali},    {0x0A00, 0x0A7F, Script::Gurmukhi},
    {0x0A80, 0x0AFF, Script::Gujarati},   {0x0B00, 0x0B7F, Script::Oriya},      {0x0B80, 0x0BFF, Script::Tamil},
    {0x0C00, 0x0C7F, Script::Telugu},     {0x0C80, 0x0CFF, Script::Kannada},    {0x0D00, 0x0D7F, Script::Malayalam},
    {0x0D80, 0x0DFF, Script::Sinhala},    {0x0E00, 0x0E7F, Script::Thai},       {0x0E80, 0x0EFF, Script::Lao},
    {0x1000, 0x109F, Script::Myanmar},    {0x10A0, 0x10FF, Script::Georgian},   {0x1100, 0x11FF, Script::Hangul},
    {0x1200, 0x139F, Script::Ethiopic},   {0x1780, 0x17FF, Script::Khmer},      {0x19E0, 0x19FF, Script::Khmer},
    {0x1C90, 0x1CBF, Script::Georgian},   {0x1CD0, 0x1CFF, Script::Devanagari}, {0x2D00, 0x2D2F, Script::Georgian},
    {0x2D80, 0x2DDF, Script::Ethiopic},   {0x2E80, 0x2FDF, Script::Han},        {0x3000, 0x303F, Script::CjkSymbol},
    {0x3040, 0x30FF, Script::Kana},       {0x3100, 0x312F, Script::Han},        {0x3130, 0x318F, Script::Hangul},
    {0x3190, 0x31BF, Script::Han},        {0x31C0, 0x31EF, Script::CjkSymbol},  {0x31F0, 0x31FF, Script::Kana},
    {0x3200, 0x33FF, Script::CjkSymbol},  {0x3400, 0x4DBF, Script::Han},        {0x4E00, 0x9FFF, Script::Han},
    {0xA8E0, 0xA8FF, Script::Devanagari}, {0xA960, 0xA97F, Script::Hangul},     {0xA9E0, 0xA9FF, Script::Myanmar},
    {0xAA60, 0xAA7F, Script::Myanmar},    {0xAB00, 0xAB2F, Script::Ethiopic},   {0xAC00, 0xD7FF, Script::Hangul},
    {0xF900, 0xFAFF, Script::Han},        {0xFB13, 0xFB17, Script::Armenian},   {0xFB1D, 0xFB4F, Script::Hebrew},
    {0xFB50, 0xFDFF, Script::Arabic},     {0xFE30, 0xFE4F, Script::CjkSymbol}, {0xFE70, 0xFEFF, Script::Arabic},
    {0xFF00, 0xFF64, Script::CjkSymbol},  {0xFF65, 0xFF9F, Script::Kana},       {0xFFA0, 0xFFDC, Script::Hangul},
    {0xFFE0, 0xFFEE, Script::CjkSymbol},  {0x111E0, 0x111FF, Script::Sinhala},  {0x11FC0, 0x11FFF, Script::Tamil},
    {0x1B000, 0x1B0FF, Script::Kana},     {0x1E7E0, 0x1E7FF, Script::Ethiopic}, {0x20000, 0x3134F, Script::Han},
};

const char* const kNotoNames[] = {"Arabic",  "Hebrew",  "Devanagari", "Bengali",  "Tamil",    "Telugu",  "Kannada",
                                  "Malayalam", "Gujarati", "Gurmukhi", "Oriya",    "Sinhala",  "Thai",    "Lao",
                                  "Khmer",   "Myanmar", "Georgian",   "Armenian", "Ethiopic", "Thaana"};
static_assert(sizeof(kNotoNames) / sizeof(kNotoNames[0]) == kNotoLast - kNotoFirst + 1, "one name per Noto family");

template <typename T, size_t N, typename First, typename Last>
const T* find_range(const T (&table)[N], uint32_t cp, First first, Last last) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (last(table[mid]) < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < N && first(table[lo]) <= cp ? &table[lo] : nullptr;
}

// Ranges that need shaping or bidi (see needs_shaping), sorted.
const uint32_t kComplex[][2] = {
    {0x0300, 0x036F},  {0x0483, 0x0489},  {0x0591, 0x08FF},  {0x0900, 0x0DFF},  {0x0E00, 0x0FFF},
    {0x1000, 0x109F},  {0x1780, 0x17FF},  {0x1A00, 0x1AFF},  {0x1CD0, 0x1CFF},  {0x1DC0, 0x1DFF},
    {0x200C, 0x200F},  {0x202A, 0x202E},  {0x2066, 0x2069},  {0x20D0, 0x20FF},  {0xA8E0, 0xA8FF},
    {0xA9E0, 0xA9FF},  {0xAA60, 0xAA7F},  {0xFB1D, 0xFDFF},  {0xFE20, 0xFE2F},  {0xFE70, 0xFEFF},
    {0x10800, 0x10FFF}, {0x1E800, 0x1EFFF},
};

}  // namespace

Script script_of(uint32_t cp) {
    if (cp < 0x0530) return Script::Common;
    const ScriptRange* r = find_range(
        kScripts, cp, [](const ScriptRange& x) { return x.first; }, [](const ScriptRange& x) { return x.last; });
    return r ? r->script : Script::Common;
}

const char* noto_family(Script s) {
    int i = (int)s;
    return i >= kNotoFirst && i <= kNotoLast ? kNotoNames[i - kNotoFirst] : nullptr;
}

bool noto_has_medium(Script s) { return noto_family(s) && s != Script::Oriya; }

bool is_mark(uint32_t cp) {
    if (cp < 0x0300) return false;
    using R = uint32_t[2];
    return find_range(
               kMarkRanges, cp, [](const R& x) { return x[0]; }, [](const R& x) { return x[1]; }) != nullptr;
}

bool is_invisible(uint32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) ||
           (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF || cp == 0x061C || cp == 0x00AD;
}

bool needs_shaping(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0xCC) continue;  // ASCII, continuation bytes, and lead bytes below U+0300
        uint32_t cp = utf8_decode(s, i);
        using R = uint32_t[2];
        if (find_range(
                kComplex, cp, [](const R& x) { return x[0]; }, [](const R& x) { return x[1]; }))
            return true;
    }
    return false;
}

bool has_kana(const std::string& s) {
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) {
        if ((unsigned char)s[i] < 0xE3) continue;
        if (script_of(utf8_decode(s, i)) == Script::Kana) return true;
    }
    return false;
}

uint32_t fold_styled(uint32_t cp, bool* bold) {
    if (bold) *bold = false;
    if (cp < 0x2100 || (cp > 0x214F && cp < 0x1D400) || cp > 0x1D7FF) return cp;
    const StyledRun* r = find_range(
        kStyledRuns, cp, [](const StyledRun& x) { return x.first; }, [](const StyledRun& x) { return x.last; });
    if (!r) return cp;
    if (bold) *bold = r->bold;
    return r->target + (cp - r->first);
}

namespace {
bool is_virama(uint32_t cp) {
    switch (cp) {
    case 0x094D: case 0x09CD: case 0x0A4D: case 0x0ACD: case 0x0B4D: case 0x0BCD: case 0x0C4D: case 0x0CCD:
    case 0x0D3B: case 0x0D3C: case 0x0D4D: case 0x0DCA: case 0x0E3A: case 0x1039: case 0x103A: case 0x17D2:
        return true;
    default:
        return false;
    }
}
}  // namespace

bool can_break_between(uint32_t prev, uint32_t cur) {
    if (is_mark(cur) || cur == 0x200C || cur == 0x200D || is_emoji_component(cur)) return false;
    if (prev == 0x200D || is_virama(prev)) return false;
    if ((prev >= 0x0E40 && prev <= 0x0E44) || (prev >= 0x0EC0 && prev <= 0x0EC4)) return false;  // leading vowels
    if (cur == 0x0E30 || cur == 0x0E32 || cur == 0x0E33 || cur == 0x0E45 || cur == 0x0EB0 || cur == 0x0EB2 ||
        cur == 0x0EB3)
        return false;  // following vowels (letters, not marks, but they end the syllable)
    return true;
}

}  // namespace vt
