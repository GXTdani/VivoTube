#include "gfx.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <vita2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/emoji.h"
#include "../core/util.h"

namespace vt {

Gfx g_gfx;

// ---------------------------------------------------------------- Roboto via FreeType

namespace {
constexpr float kPxPerScale = 18.0f;  // Roboto pixel size at scale 1.0
constexpr int kAtlasW = 1024, kAtlasH = 1024;
const char* const kFontFiles[3] = {"app0:assets/fonts/Roboto-Regular.ttf", "app0:assets/fonts/Roboto-Medium.ttf",
                                   "app0:assets/fonts/Roboto-Bold.ttf"};
}  // namespace

// Glyphs are rendered on demand into one 8-bit atlas and drawn as one batch of quads per string.
struct FontEngine {
    struct Glyph {
        int16_t ax = 0, ay = 0, w = 0, h = 0, left = 0, top = 0;
        float adv = 0;
        bool missing = false;  // not in Roboto: the system font draws it
        bool placed = false;   // bitmap is in the atlas
    };

    FT_Library lib = nullptr;
    FT_Face face[3] = {nullptr, nullptr, nullptr};
    std::string data[3];
    int sizeOf[3] = {0, 0, 0};
    float ascEm = 0.928f, descEm = 0.244f;
    vita2d_texture* atlas = nullptr;
    uint8_t* pix = nullptr;
    unsigned stride = 0;
    int penX = 1, penY = 1, shelfH = 0;
    bool full = false;
    uint8_t gamma[256];
    std::unordered_map<uint32_t, Glyph> glyphs;

    bool init() {
        if (FT_Init_FreeType(&lib)) return false;
        for (int i = 0; i < 3; i++) {
            if (!read_file(kFontFiles[i], data[i]) ||
                FT_New_Memory_Face(lib, (const FT_Byte*)data[i].data(), (FT_Long)data[i].size(), 0, &face[i])) {
                VT_LOG("font: cannot load %s", kFontFiles[i]);
                return false;
            }
        }
        FT_Face f = face[0];
        if (f->units_per_EM > 0) {
            ascEm = (float)f->ascender / f->units_per_EM;
            descEm = (float)-f->descender / f->units_per_EM;
        }
        atlas = vita2d_create_empty_texture_format(kAtlasW, kAtlasH, SCE_GXM_TEXTURE_FORMAT_U8_R111);
        if (!atlas) return false;
        vita2d_texture_set_filters(atlas, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        pix = (uint8_t*)vita2d_texture_get_datap(atlas);
        stride = vita2d_texture_get_stride(atlas);
        memset(pix, 0, (size_t)stride * kAtlasH);
        // Light text on a dark page reads thin with linear coverage; lift the midtones a little.
        for (int i = 0; i < 256; i++) gamma[i] = (uint8_t)std::lround(255.0 * std::pow(i / 255.0, 0.82));
        return true;
    }

    void shutdown() {
        if (atlas) {
            vita2d_wait_rendering_done();
            vita2d_free_texture(atlas);
        }
        atlas = nullptr;
        for (auto& f : face) {
            if (f) FT_Done_Face(f);
            f = nullptr;
        }
        if (lib) FT_Done_FreeType(lib);
        lib = nullptr;
    }

    void recycle() {
        if (!full) return;
        vita2d_wait_rendering_done();
        memset(pix, 0, (size_t)stride * kAtlasH);
        glyphs.clear();
        penX = penY = 1;
        shelfH = 0;
        full = false;
        VT_LOG("font: atlas recycled");
    }

    const Glyph& get(int w, int px, uint32_t cp) {
        uint32_t key = ((uint32_t)w << 30) | ((uint32_t)px << 21) | (cp & 0x1FFFFF);
        auto it = glyphs.find(key);
        if (it != glyphs.end() && (it->second.placed || it->second.missing || it->second.w == 0 || full))
            return it->second;
        Glyph& g = glyphs[key];
        g = Glyph();
        FT_Face f = face[w];
        FT_UInt idx = FT_Get_Char_Index(f, cp);
        if (idx == 0) {
            g.missing = true;
            return g;
        }
        if (sizeOf[w] != px) {
            FT_Set_Pixel_Sizes(f, 0, px);
            sizeOf[w] = px;
        }
        if (FT_Load_Glyph(f, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
            g.missing = true;
            return g;
        }
        FT_GlyphSlot sl = f->glyph;
        const FT_Bitmap& bm = sl->bitmap;
        g.adv = sl->linearHoriAdvance / 65536.0f;
        g.left = (int16_t)sl->bitmap_left;
        g.top = (int16_t)sl->bitmap_top;
        g.w = (int16_t)bm.width;
        g.h = (int16_t)bm.rows;
        if (g.w == 0 || g.h == 0) return g;
        if (penX + g.w + 1 > kAtlasW) {
            penX = 1;
            penY += shelfH + 1;
            shelfH = 0;
        }
        if (penY + g.h + 1 > kAtlasH || g.w + 2 > kAtlasW) {
            full = true;  // drawn from the next frame on, after the atlas is recycled
            return g;
        }
        for (int r = 0; r < g.h; r++) {
            const uint8_t* src = bm.buffer + r * bm.pitch;
            uint8_t* dst = pix + (size_t)(penY + r) * stride + penX;
            for (int c = 0; c < g.w; c++) dst[c] = gamma[src[c]];
        }
        g.ax = (int16_t)penX;
        g.ay = (int16_t)penY;
        g.placed = true;
        penX += g.w + 1;
        shelfH = std::max(shelfH, (int)g.h);
        return g;
    }
};

// ---------------------------------------------------------------- emoji images

// Twemoji images from assets/emoji/emoji.pak, decoded on demand into one RGBA atlas of padded
// cells. Like the glyph atlas it is cleared and refilled when it runs out of room.
struct EmojiAtlas {
    static constexpr int kTex = 512, kCell = 34, kMax = kCell - 2, kCols = kTex / kCell;
    static constexpr int kCells = kCols * kCols, kDecodesPerFrame = 40;
    struct Slot {
        int cell = -1, size = 0;  // cell -1: the image could not be decoded
    };
    EmojiPack pack;
    vita2d_texture* tex = nullptr;
    uint32_t* pix = nullptr;
    unsigned stride = 0;  // in pixels
    std::unordered_map<int, Slot> slots;
    int next = 0, decoded = 0;
    bool full = false;

    bool init() {
        std::string err;
        if (!pack.open("app0:assets/emoji/emoji.pak", &err)) {
            VT_LOG("emoji: %s", err.c_str());
            return false;
        }
        tex = vita2d_create_empty_texture(kTex, kTex);
        if (!tex) return false;
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        pix = (uint32_t*)vita2d_texture_get_datap(tex);
        stride = vita2d_texture_get_stride(tex) / 4;
        memset(pix, 0, (size_t)stride * 4 * kTex);
        VT_LOG("emoji: %u images", (unsigned)pack.size());
        return true;
    }

    void shutdown() {
        if (tex) {
            vita2d_wait_rendering_done();
            vita2d_free_texture(tex);
        }
        tex = nullptr;
    }

    void beginFrame() {
        decoded = 0;
        if (!full) return;
        vita2d_wait_rendering_done();
        memset(pix, 0, (size_t)stride * 4 * kTex);
        slots.clear();
        next = 0;
        full = false;
        VT_LOG("emoji: atlas recycled");
    }

    // The atlas cell holding pack entry `index`, decoding it if needed; nullptr when it can't be
    // drawn this frame (out of room or over this frame's decode budget).
    const Slot* get(int index) {
        auto it = slots.find(index);
        if (it != slots.end()) return it->second.cell >= 0 ? &it->second : nullptr;
        if (full || decoded >= kDecodesPerFrame) return nullptr;
        if (next >= kCells) {
            full = true;
            return nullptr;
        }
        decoded++;
        Slot& sl = slots[index];
        std::string data;
        static uint32_t img[kMax * kMax];
        // Decoded with vita2d's loader into a scratch texture (libpng's simplified reader
        // crashed here), then copied into the atlas.
        if (!pack.png(index, data) || data.size() < 8 || memcmp(data.data(), "\x89PNG", 4) != 0) return nullptr;
        vita2d_texture* t = vita2d_load_PNG_buffer(data.data());
        if (!t) return nullptr;
        int w = (int)vita2d_texture_get_width(t), h = (int)vita2d_texture_get_height(t);
        if (w != h || w > kMax || w < 4) {
            vita2d_free_texture(t);
            return nullptr;
        }
        const uint32_t* src = (const uint32_t*)vita2d_texture_get_datap(t);
        unsigned srcStride = vita2d_texture_get_stride(t) / 4;
        for (int y = 0; y < h; y++) memcpy(img + y * w, src + (size_t)y * srcStride, (size_t)w * 4);
        vita2d_free_texture(t);  // never drawn, so it can go right away
        // Give fully transparent pixels a neighbour's colour so filtering doesn't darken the edges.
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t& p = img[y * w + x];
                if (p >> 24) continue;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    uint32_t q = img[ny[k] * w + nx[k]];
                    if (q >> 24) {
                        p = q & 0x00FFFFFFu;
                        break;
                    }
                }
            }
        int cx = (next % kCols) * kCell + 1, cy = (next / kCols) * kCell + 1;
        for (int y = 0; y < h; y++) memcpy(pix + (size_t)(cy + y) * stride + cx, img + y * w, (size_t)w * 4);
        sl.cell = next++;
        sl.size = w;
        return &sl;
    }
};

namespace {
int px_for(float scale) {
    int px = (int)std::lround(scale * kPxPerScale);
    return px < 6 ? 6 : (px > 200 ? 200 : px);
}
}  // namespace

bool Gfx::init() {
    font_ = vita2d_load_default_pgf();
    ft_ = new FontEngine();
    if (!ft_->init()) {
        ft_->shutdown();
        delete ft_;
        ft_ = nullptr;
    }
    if (!font_ && !ft_) return false;
    if (ft_) {
        emoji_ = new EmojiAtlas();
        if (!emoji_->init()) {
            emoji_->shutdown();
            delete emoji_;
            emoji_ = nullptr;
        }
        lineH_ = (ft_->ascEm + ft_->descEm) * kPxPerScale;
        ascent_ = ft_->ascEm * kPxPerScale;
        VT_LOG("gfx: roboto, line height %.1f", lineH_);
    } else {
        int h = vita2d_pgf_text_height(font_, 1.0f, "Ag");
        float glyph = h > 4 ? (float)h : 20.0f;
        lineH_ = glyph * 1.22f;
        ascent_ = (lineH_ - glyph) * 0.5f + glyph * 0.80f;
        VT_LOG("gfx: pgf line height %.1f", lineH_);
    }
    return true;
}

void Gfx::shutdown() {
    if (emoji_) {
        emoji_->shutdown();
        delete emoji_;
        emoji_ = nullptr;
    }
    if (ft_) {
        ft_->shutdown();
        delete ft_;
        ft_ = nullptr;
    }
    if (font_) vita2d_free_pgf(font_);
    font_ = nullptr;
}

void Gfx::beginFrame() {
    if (ft_) ft_->recycle();
    if (emoji_) emoji_->beginFrame();
}


// Lays out one line. Draws it when `draw` is set; returns the advance width.
float Gfx::measure(const std::string& s, float scale, int weight, bool draw, float x, float y, uint32_t color) {
    if (s.empty()) return 0;
    if (!ft_) {
        int base = (int)(y + ascent_ * scale);
        if (draw) {
            vita2d_pgf_draw_text(font_, (int)x, base, color, scale, s.c_str());
            if (weight) vita2d_pgf_draw_text(font_, (int)x + 1, base, color, scale, s.c_str());
        }
        return (float)vita2d_pgf_text_width(font_, scale, s.c_str()) + (weight ? 1 : 0);
    }
    int w = weight < 0 ? 0 : (weight > 2 ? 2 : weight);
    int px = px_for(scale);
    float pgfScale = px / 18.5f;
    float pen = draw ? std::round(x) : 0;
    float start = pen;
    // Center the rounded size's ascent + descent in the nominal line box.
    float baseline = std::round(y + (lineH_ * scale - (ft_->ascEm + ft_->descEm) * px) * 0.5f + ft_->ascEm * px);
    vita2d_texture_vertex* v = nullptr;
    size_t nv = 0;
    if (draw) {
        v = (vita2d_texture_vertex*)vita2d_pool_memalign((unsigned)(s.size() * 6 * sizeof(vita2d_texture_vertex)),
                                                         sizeof(vita2d_texture_vertex));
    }
    const float iw = 1.0f / kAtlasW, ih = 1.0f / kAtlasH;
    std::string fallback;
    float fallbackX = 0;
    auto flush = [&] {
        if (fallback.empty()) return;
        if (font_) {
            if (draw) vita2d_pgf_draw_text(font_, (int)fallbackX, (int)baseline, color, pgfScale, fallback.c_str());
            pen = fallbackX + vita2d_pgf_text_width(font_, pgfScale, fallback.c_str());
        }
        fallback.clear();
    };
    // Emoji: a little larger than the letters, centred on the middle of the capitals.
    vita2d_texture_vertex* ev = nullptr;
    size_t nev = 0;
    const float es = std::round(px * 1.12f), eAdv = es + std::round(px * 0.12f);
    const float eTop = std::round(baseline - px * 0.36f - es * 0.5f);
    const float ei = 1.0f / EmojiAtlas::kTex;
    for (size_t i = 0, next = 0; i < s.size(); i = next) {
        next = utf8_next(s, i);
        uint32_t cp = utf8_decode(s, i);
        if (cp == '\n' || cp == '\r') continue;
        if (cp == '\t' || cp == 0xA0) cp = ' ';
        if (emoji_ && cp >= '#') {
            size_t end = 0;
            bool textStyle = false;
            int idx = emoji_->pack.match(s, i, &end, &textStyle);
            // A plain symbol such as the copyright sign or the play triangle stays text.
            if (idx >= 0 && !textStyle) {
                flush();
                if (draw) {
                    const EmojiAtlas::Slot* sl = emoji_->get(idx);
                    if (sl && !ev) {
                        ev = (vita2d_texture_vertex*)vita2d_pool_memalign(
                            (unsigned)((s.size() / 2 + 1) * 6 * sizeof(vita2d_texture_vertex)),
                            sizeof(vita2d_texture_vertex));
                    }
                    if (sl && ev) {
                        float gx = std::round(pen + (eAdv - es) * 0.5f), gy = eTop, x1 = gx + es, y1 = gy + es;
                        int cx = (sl->cell % EmojiAtlas::kCols) * EmojiAtlas::kCell + 1;
                        int cy = (sl->cell / EmojiAtlas::kCols) * EmojiAtlas::kCell + 1;
                        float u0 = cx * ei, v0 = cy * ei, u1 = (cx + sl->size) * ei, v1 = (cy + sl->size) * ei;
                        vita2d_texture_vertex q[6] = {{gx, gy, 0.5f, u0, v0}, {x1, gy, 0.5f, u1, v0},
                                                      {gx, y1, 0.5f, u0, v1}, {gx, y1, 0.5f, u0, v1},
                                                      {x1, gy, 0.5f, u1, v0}, {x1, y1, 0.5f, u1, v1}};
                        memcpy(ev + nev, q, sizeof(q));
                        nev += 6;
                    }
                }
                pen += eAdv;
                next = end;
                continue;
            }
        }
        // Joiners, selectors, tones and pictographs the pack lacks: no font here draws them.
        if (is_emoji_component(cp) || is_pictograph(cp)) continue;
        const FontEngine::Glyph& g = ft_->get(w, px, cp);
        if (g.missing) {
            if (fallback.empty()) fallbackX = pen;
            fallback += s.substr(i, next - i);
            continue;
        }
        flush();
        if (draw && v && g.placed) {
            float gx = std::round(pen) + g.left, gy = baseline - g.top;
            float u0 = g.ax * iw, v0 = g.ay * ih, u1 = (g.ax + g.w) * iw, v1 = (g.ay + g.h) * ih;
            float x1 = gx + g.w, y1 = gy + g.h;
            vita2d_texture_vertex q[6] = {{gx, gy, 0.5f, u0, v0}, {x1, gy, 0.5f, u1, v0}, {gx, y1, 0.5f, u0, v1},
                                          {gx, y1, 0.5f, u0, v1}, {x1, gy, 0.5f, u1, v0}, {x1, y1, 0.5f, u1, v1}};
            memcpy(v + nv, q, sizeof(q));
            nv += 6;
        }
        pen += g.adv;
    }
    flush();
    if (draw && v && nv) vita2d_draw_array_textured(ft_->atlas, SCE_GXM_PRIMITIVE_TRIANGLES, v, nv, color);
    if (draw && ev && nev) {
        vita2d_draw_array_textured(emoji_->tex, SCE_GXM_PRIMITIVE_TRIANGLES, ev, nev,
                                   with_alpha(0xFFFFFFFFu, (uint8_t)(color >> 24)));
    }
    return pen - start;
}

void Gfx::text(float x, float y, uint32_t color, float scale, const std::string& s, int weight) {
    measure(s, scale, weight, true, x, y, color);
}

void Gfx::textCentered(float cx, float y, uint32_t color, float scale, const std::string& s, int weight) {
    text(cx - width(s, scale, weight) / 2.0f, y, color, scale, s, weight);
}

void Gfx::textRight(float rx, float y, uint32_t color, float scale, const std::string& s, int weight) {
    text(rx - width(s, scale, weight), y, color, scale, s, weight);
}

int Gfx::width(const std::string& s, float scale, int weight) {
    if (s.empty()) return 0;
    return (int)std::ceil(measure(s, scale, weight, false, 0, 0, 0));
}

std::string Gfx::ellipsize(const std::string& s, float maxWidth, float scale, int weight) {
    if (width(s, scale, weight) <= maxWidth) return s;
    // Binary search on code point boundaries.
    std::vector<size_t> bounds;
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) bounds.push_back(i);
    size_t lo = 0, hi = bounds.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (width(s.substr(0, bounds[mid]) + "...", scale, weight) <= maxWidth) lo = mid;
        else hi = mid;
    }
    return trim(s.substr(0, bounds.empty() ? 0 : bounds[lo])) + "...";
}

const std::vector<std::string>& Gfx::wrap(const std::string& s, float maxWidth, float scale, int maxLines,
                                          int weight) {
    std::string key = s;
    key += str_printf("\x1f%d\x1f%.2f\x1f%d\x1f%d", (int)maxWidth, scale, maxLines, weight);
    auto it = wrapCache_.find(key);
    if (it != wrapCache_.end()) return it->second;
    if (wrapCache_.size() > 2500) wrapCache_.clear();

    std::vector<std::string> lines;
    bool truncated = false;
    size_t pstart = 0;
    while (pstart <= s.size() && !truncated) {
        size_t pend = s.find('\n', pstart);
        if (pend == std::string::npos) pend = s.size();
        std::string para = s.substr(pstart, pend - pstart);
        // Tokens: words separated by spaces; CJK characters are tokens of their own.
        std::vector<std::pair<std::string, bool>> toks;  // token, preceded by a space
        std::string cur;
        bool curSpace = false, pendingSpace = false;
        for (size_t i = 0; i < para.size(); i = utf8_next(para, i)) {
            uint32_t cp = utf8_decode(para, i);
            size_t n = utf8_next(para, i) - i;
            if (cp == ' ' || cp == '\t' || cp == '\r') {
                if (!cur.empty()) toks.push_back({cur, curSpace});
                cur.clear();
                pendingSpace = true;
            } else if (is_cjk(cp)) {
                if (!cur.empty()) {
                    toks.push_back({cur, curSpace});
                    cur.clear();
                }
                toks.push_back({para.substr(i, n), pendingSpace});
                pendingSpace = false;
            } else {
                if (cur.empty()) {
                    curSpace = pendingSpace;
                    pendingSpace = false;
                }
                cur += para.substr(i, n);
            }
        }
        if (!cur.empty()) toks.push_back({cur, curSpace});

        std::string line;
        for (auto& tk : toks) {
            std::string cand = line.empty() ? tk.first : line + (tk.second ? " " : "") + tk.first;
            if (width(cand, scale, weight) <= maxWidth) {
                line = cand;
                continue;
            }
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
                if ((int)lines.size() >= maxLines) {
                    truncated = true;
                    break;
                }
            }
            // Token wider than the line: hard-break by code point.
            std::string t = tk.first;
            while (width(t, scale, weight) > maxWidth) {
                size_t cut = 0;
                for (size_t i = 0; i < t.size(); i = utf8_next(t, i)) {
                    size_t nx = utf8_next(t, i);
                    if (width(t.substr(0, nx), scale, weight) > maxWidth) break;
                    cut = nx;
                }
                if (cut == 0) cut = utf8_next(t, 0);
                lines.push_back(t.substr(0, cut));
                t = t.substr(cut);
                if ((int)lines.size() >= maxLines) {
                    truncated = true;
                    break;
                }
            }
            if (truncated) break;
            line = t;
        }
        if (!truncated) {
            if (!line.empty() || para.empty()) lines.push_back(line);
            if ((int)lines.size() >= maxLines && pend < s.size()) truncated = true;
            if ((int)lines.size() > maxLines) lines.resize(maxLines);
        }
        if (pend >= s.size()) break;
        pstart = pend + 1;
    }
    if (truncated && !lines.empty()) {
        if ((int)lines.size() > maxLines) lines.resize(maxLines);
        lines.back() = ellipsize(lines.back() + " ...", maxWidth, scale, weight);
        if (lines.back().size() >= 4 && lines.back().compare(lines.back().size() - 4, 4, " ...") == 0)
            lines.back().replace(lines.back().size() - 4, 4, "...");
    }
    // Drop trailing empty lines.
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return wrapCache_.emplace(key, std::move(lines)).first->second;
}

float Gfx::paragraph(float x, float y, float maxWidth, uint32_t color, float scale, const std::string& s, int maxLines,
                     float lineGap, int weight) {
    const std::vector<std::string>& lines = wrap(s, maxWidth, scale, maxLines, weight);
    float lh = lineHeight(scale) * lineGap;
    for (size_t i = 0; i < lines.size(); i++) text(x, y + i * lh, color, scale, lines[i], weight);
    return lines.size() * lh;
}

// ---------------------------------------------------------------- shapes

void Gfx::rect(float x, float y, float w, float h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    vita2d_draw_rectangle(x, y, w, h, color);
}

namespace {

// Triangle lists from the vita2d frame pool (released when the frame is done).
struct Tris {
    vita2d_color_vertex* v = nullptr;
    size_t n = 0, cap = 0;
    explicit Tris(size_t triangles) : cap(triangles * 3) {
        v = (vita2d_color_vertex*)vita2d_pool_memalign((unsigned)(cap * sizeof(vita2d_color_vertex)),
                                                       sizeof(vita2d_color_vertex));
    }
    void add(float x, float y, uint32_t c) {
        if (!v || n >= cap) return;
        v[n].x = x;
        v[n].y = y;
        v[n].z = 0.5f;
        v[n].color = c;
        n++;
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
        add(x0, y0, c);
        add(x1, y1, c);
        add(x2, y2, c);
    }
    void draw() {
        if (v && n >= 3) vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, n - n % 3);
    }
};

const float kPi = 3.14159265f;
const float kHalfPi = 1.57079633f;
const float kCornerStart[4] = {kPi, kPi + kHalfPi, 0.0f, kHalfPi};  // TL, TR, BR, BL

int arc_segments(float r) {
    int s = (int)(r * 0.6f);
    return s < 3 ? 3 : (s > 12 ? 12 : s);
}

// Perimeter of a rounded rectangle, clockwise from the top-left corner.
void round_outline(float x, float y, float w, float h, float r, int seg, std::vector<float>& pts) {
    pts.clear();
    float maxR = std::min(w, h) * 0.5f;
    if (r > maxR) r = maxR;
    if (r < 0) r = 0;
    const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= seg; i++) {
            float a = kCornerStart[c] + kHalfPi * i / seg;
            pts.push_back(cx[c] + std::cos(a) * r);
            pts.push_back(cy[c] + std::sin(a) * r);
        }
    }
}

}  // namespace

void Gfx::roundRect(float x, float y, float w, float h, float r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (r < 0.5f) {
        rect(x, y, w, h, color);
        return;
    }
    std::vector<float> p;
    round_outline(x, y, w, h, r, arc_segments(r), p);
    size_t n = p.size() / 2;
    float mx = x + w / 2, my = y + h / 2;
    Tris t(n);
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        t.tri(mx, my, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], color);
    }
    t.draw();
}

void Gfx::roundFrame(float x, float y, float w, float h, float r, float th, uint32_t color) {
    if (w <= 2 * th || h <= 2 * th) return;
    int seg = arc_segments(std::max(r, 1.0f));
    std::vector<float> o, in;
    round_outline(x, y, w, h, r, seg, o);
    round_outline(x + th, y + th, w - 2 * th, h - 2 * th, std::max(0.0f, r - th), seg, in);
    size_t n = o.size() / 2;
    Tris t(n * 2);
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        t.tri(o[i * 2], o[i * 2 + 1], o[j * 2], o[j * 2 + 1], in[i * 2], in[i * 2 + 1], color);
        t.tri(in[i * 2], in[i * 2 + 1], o[j * 2], o[j * 2 + 1], in[j * 2], in[j * 2 + 1], color);
    }
    t.draw();
}

void Gfx::cornerMask(float x, float y, float w, float h, float r, uint32_t bgColor) {
    float maxR = std::min(w, h) * 0.5f;
    if (r > maxR) r = maxR;
    if (r < 0.5f) return;
    int seg = arc_segments(r);
    const float px[4] = {x, x + w, x + w, x};  // the square corners
    const float py[4] = {y, y, y + h, y + h};
    const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    Tris t(4 * seg);
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < seg; i++) {
            float a0 = kCornerStart[c] + kHalfPi * i / seg, a1 = kCornerStart[c] + kHalfPi * (i + 1) / seg;
            t.tri(px[c], py[c], cx[c] + std::cos(a0) * r, cy[c] + std::sin(a0) * r, cx[c] + std::cos(a1) * r,
                  cy[c] + std::sin(a1) * r, bgColor);
        }
    }
    t.draw();
}

void Gfx::circle(float cx, float cy, float r, uint32_t color) { roundRect(cx - r, cy - r, 2 * r, 2 * r, r, color); }

void Gfx::ring(float cx, float cy, float r, float th, uint32_t color) {
    roundFrame(cx - r, cy - r, 2 * r, 2 * r, r, th, color);
}

void Gfx::line(float x0, float y0, float x1, float y1, float th, uint32_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    Tris t(2);
    t.tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, color);
    t.tri(x0 - nx, y0 - ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color);
    t.draw();
}

void Gfx::triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
    Tris t(1);
    t.tri(x0, y0, x1, y1, x2, y2, color);
    t.draw();
}

void Gfx::vgradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    Tris t(2);
    t.add(x, y, top);
    t.add(x + w, y, top);
    t.add(x, y + h, bottom);
    t.add(x, y + h, bottom);
    t.add(x + w, y, top);
    t.add(x + w, y + h, bottom);
    t.draw();
}

void Gfx::hgradient(float x, float y, float w, float h, uint32_t left, uint32_t right) {
    if (w <= 0 || h <= 0) return;
    Tris t(2);
    t.add(x, y, left);
    t.add(x + w, y, right);
    t.add(x, y + h, left);
    t.add(x, y + h, left);
    t.add(x + w, y, right);
    t.add(x + w, y + h, right);
    t.draw();
}

void Gfx::image(vita2d_texture* tex, float x, float y, float w, float h) {
    if (!tex) return;
    float tw = (float)vita2d_texture_get_width(tex), th = (float)vita2d_texture_get_height(tex);
    vita2d_draw_texture_scale(tex, x, y, w / tw, h / th);
}

void Gfx::imageCover(vita2d_texture* tex, float x, float y, float w, float h) {
    if (!tex) return;
    float tw = (float)vita2d_texture_get_width(tex), th = (float)vita2d_texture_get_height(tex);
    float scale = std::max(w / tw, h / th);
    float sw = w / scale, sh = h / scale;
    vita2d_draw_texture_part_scale(tex, x, y, (tw - sw) / 2, (th - sh) / 2, sw, sh, scale, scale);
}

void Gfx::spinner(float cx, float cy, float r, uint32_t color) {
    // A ring with a sweeping arc, like a material progress indicator.
    ring(cx, cy, r, 3, with_alpha(color, 50));
    const int n = 14;
    float head = (float)std::fmod(time * 5.5, 6.2831853);
    for (int i = 0; i < n; i++) {
        float a = head - i * 0.17f;
        uint8_t alpha = (uint8_t)(255 - i * 14);
        circle(cx + std::cos(a) * (r - 1.5f), cy + std::sin(a) * (r - 1.5f), 1.8f, with_alpha(color, alpha));
    }
}

void Gfx::clip(const Rect& r) {
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)r.x, (int)r.y, (int)(r.x + r.w), (int)(r.y + r.h));
}

void Gfx::unclip() { vita2d_disable_clipping(); }

}  // namespace vt
