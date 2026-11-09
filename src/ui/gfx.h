// Drawing helpers over vita2d: colors, the system font, cached text layout, clipping, and
// rounded shapes built from triangle lists.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct vita2d_pgf;
struct vita2d_texture;

namespace vt {

struct FontEngine;
struct EmojiAtlas;
struct TextCache;

constexpr int SCREEN_W = 960;
constexpr int SCREEN_H = 544;

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
constexpr uint32_t with_alpha(uint32_t c, uint8_t a) { return (c & 0x00FFFFFFu) | ((uint32_t)a << 24); }

// Dark theme in the style of the familiar video-site layout.
namespace col {
constexpr uint32_t bg = rgba(15, 15, 15);           // page background
constexpr uint32_t raised = rgba(39, 39, 39);       // chips, secondary buttons, boxes
constexpr uint32_t raisedHi = rgba(63, 63, 63);     // focused fill on raised surfaces
constexpr uint32_t menu = rgba(40, 40, 40);         // menus and dialogs
constexpr uint32_t menuSel = rgba(66, 66, 66);
constexpr uint32_t divider = rgba(255, 255, 255, 28);
constexpr uint32_t text = rgba(241, 241, 241);
constexpr uint32_t text2 = rgba(170, 170, 170);
constexpr uint32_t text3 = rgba(113, 113, 113);
constexpr uint32_t onLight = rgba(15, 15, 15);      // text on white buttons and chips
constexpr uint32_t red = rgba(255, 0, 51);          // progress bars, live badges (#FF0033)
constexpr uint32_t redPink = rgba(255, 39, 145);    // right end of the player's progress gradient
constexpr uint32_t blue = rgba(62, 166, 255);       // links and switches
constexpr uint32_t focus = rgba(241, 241, 241);     // d-pad focus ring
constexpr uint32_t badge = rgba(0, 0, 0, 204);      // duration badges
constexpr uint32_t thumbBg = rgba(39, 39, 39);
constexpr uint32_t searchBg = rgba(18, 18, 18);
constexpr uint32_t searchEdge = rgba(48, 48, 48);
constexpr uint32_t searchBtn = rgba(34, 34, 34);
// VivoTube's own mark (see sce_sys/icon0.png): teal-blue tile, white ring, coral V.
constexpr uint32_t brandTop = rgba(38, 198, 190);
constexpr uint32_t brandBottom = rgba(40, 110, 220);
constexpr uint32_t brandV = rgba(255, 99, 82);
}  // namespace col

// Font weights for the bundled Roboto faces. `true` passed for a weight means Medium.
enum Weight : int { W_REGULAR = 0, W_MEDIUM = 1, W_BOLD = 2 };

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

class Gfx {
public:
    bool init();
    void shutdown();

    // Called once per frame before drawing (recycles the glyph atlas when it filled up).
    void beginFrame();

    // Text is positioned by the top of its line box. Roboto at 18 px per unit of scale; Noto Sans
    // for Arabic, Hebrew, Indic, Southeast Asian and other scripts (shaped with HarfBuzz and put in
    // order with FriBidi); the Vita's own fonts for Japanese, Chinese, Korean and symbols; and
    // Twemoji images for emoji.
    void text(float x, float y, uint32_t color, float scale, const std::string& s, int weight = W_REGULAR);
    // Medium weight (titles, buttons).
    void textBold(float x, float y, uint32_t color, float scale, const std::string& s) {
        text(x, y, color, scale, s, W_MEDIUM);
    }
    void textCentered(float cx, float y, uint32_t color, float scale, const std::string& s, int weight = W_REGULAR);
    // One line of a wrapped paragraph in a box `maxWidth` wide: on the left, or on the right
    // when the line reads right to left.
    void textLine(float x, float y, float maxWidth, uint32_t color, float scale, const std::string& s,
                  int weight = W_REGULAR);
    void textRight(float rx, float y, uint32_t color, float scale, const std::string& s, int weight = W_REGULAR);
    int width(const std::string& s, float scale, int weight = W_REGULAR);
    // Whether s reads right to left (its first strong character is Arabic, Hebrew...).
    bool rtl(const std::string& s) const;
    float lineHeight(float scale) const { return lineH_ * scale; }

    // Word-wrapped lines (cached). Long text is cut to maxLines with "...".
    const std::vector<std::string>& wrap(const std::string& s, float maxWidth, float scale, int maxLines,
                                         int weight = W_REGULAR);
    // Draws wrapped text (right-to-left paragraphs aligned right); returns height used.
    float paragraph(float x, float y, float maxWidth, uint32_t color, float scale, const std::string& s, int maxLines,
                    float lineGap = 1.0f, int weight = W_REGULAR);
    std::string ellipsize(const std::string& s, float maxWidth, float scale, int weight = W_REGULAR);

    void rect(float x, float y, float w, float h, uint32_t color);
    void roundRect(float x, float y, float w, float h, float r, uint32_t color);
    // Outline of a rounded rectangle, t pixels thick, inside the given bounds.
    void roundFrame(float x, float y, float w, float h, float r, float t, uint32_t color);
    // Paints the outside of rounded corners with bgColor (rounds images drawn underneath).
    void cornerMask(float x, float y, float w, float h, float r, uint32_t bgColor);
    void circle(float cx, float cy, float r, uint32_t color);
    void ring(float cx, float cy, float r, float t, uint32_t color);
    void line(float x0, float y0, float x1, float y1, float t, uint32_t color);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color);
    void vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
    void hgradient(float x, float y, float w, float h, uint32_t left, uint32_t right);

    void image(vita2d_texture* tex, float x, float y, float w, float h);       // stretched to fit
    void imageCover(vita2d_texture* tex, float x, float y, float w, float h);  // cropped to fill
    void spinner(float cx, float cy, float r, uint32_t color);
    void clip(const Rect& r);
    void unclip();

    uint32_t frameNo = 0;
    double time = 0;

private:
    float measure(const std::string& s, float scale, int weight, bool draw, float x, float y, uint32_t color,
                  bool keep = true);
    int trialWidth(const std::string& s, float scale, int weight);  // not kept in the shaping cache
    vita2d_pgf* font_ = nullptr;
    FontEngine* ft_ = nullptr;
    EmojiAtlas* emoji_ = nullptr;
    TextCache* cache_ = nullptr;
    float lineH_ = 21, ascent_ = 16;
    std::unordered_map<std::string, std::vector<std::string>> wrapCache_;
};

extern Gfx g_gfx;

}  // namespace vt
