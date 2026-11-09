// Unicode helpers for the text renderer: which bundled or system font family draws a character,
// which strings need shaping, cluster boundaries for breaking long words, and folding of
// "styled" letters (𝐁𝐨𝐥𝐝, 𝓈𝒸𝓇𝒾𝓅𝓉) that no font on the Vita has. Platform-neutral.
#pragma once

#include <cstdint>
#include <string>

namespace vt {

// Font families, in the order the renderer's font table lists them. Latin, Greek and Cyrillic
// come from Roboto (Common); the Noto families are bundled; Kana, Han, Hangul and CjkSymbol use
// the Vita's own system fonts.
enum class Script : uint8_t {
    Common,
    Arabic,
    Hebrew,
    Devanagari,
    Bengali,
    Tamil,
    Telugu,
    Kannada,
    Malayalam,
    Gujarati,
    Gurmukhi,
    Oriya,
    Sinhala,
    Thai,
    Lao,
    Khmer,
    Myanmar,
    Georgian,
    Armenian,
    Ethiopic,
    Thaana,
    Kana,
    Han,
    Hangul,
    CjkSymbol,
    Count
};
constexpr int kNotoFirst = (int)Script::Arabic, kNotoLast = (int)Script::Thaana;

Script script_of(uint32_t cp);
// The Noto family name for a bundled script ("Arabic" for NotoSansArabic-*.ttf), or nullptr.
const char* noto_family(Script s);
// Whether the bundled family has a Medium weight (otherwise Regular is emboldened).
bool noto_has_medium(Script s);

bool is_mark(uint32_t cp);  // combining mark (Mn, Mc, Me)
// Format characters that take no space and draw nothing: bidi marks and controls, zero-width
// space and joiners, the word joiner, the BOM, the Arabic letter mark.
bool is_invisible(uint32_t cp);
// Strings that need HarfBuzz and the bidi algorithm: right-to-left text, Indic and Southeast
// Asian scripts, combining marks, and bidi controls. Everything else is laid out a character at
// a time.
bool needs_shaping(const std::string& s);
bool has_kana(const std::string& s);  // Japanese, so Han characters use the Japanese font

// The plain letter or digit a mathematical or letterlike styled character stands for (𝐀 → A,
// ℝ → R), with *bold set for the bold styles; other characters are returned unchanged.
uint32_t fold_styled(uint32_t cp, bool* bold = nullptr);

// Whether a line may be broken between prev and cur when a word has to be split: never inside
// a cluster (before a mark or joiner, after a virama or joiner, after a Thai or Lao leading
// vowel, before a Thai or Lao following vowel).
bool can_break_between(uint32_t prev, uint32_t cur);

}  // namespace vt
