#include "gfx.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <fribidi.h>
#include <hb-ot.h>
#include <hb.h>
#include <vita2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/emoji.h"
#include "../core/text.h"
#include "../core/util.h"

namespace vt {

Gfx g_gfx;

// ---------------------------------------------------------------- fonts

namespace {
constexpr float kPxPerScale = 18.0f;  // Roboto pixel size at scale 1.0
constexpr int kAtlasW = 1024, kAtlasH = 1024;
constexpr int kNoto = kNotoLast - kNotoFirst + 1;

// The font table: Roboto in three weights, a Regular and a Medium face for each bundled Noto
// family, then the Vita's own fonts for Japanese, Chinese and Korean (regular and bold each).
enum : int {
    F_ROBOTO = 0,
    F_NOTO = 3,
    F_JPN = F_NOTO + 2 * kNoto,
    F_CN = F_JPN + 2,
    F_KR = F_CN + 2,
    F_COUNT = F_KR + 2,
};
constexpr int F_PGF = -1;  // no font here has the character: the system's PGF font draws it

const char* const kRoboto[3] = {"app0:assets/fonts/Roboto-Regular.ttf", "app0:assets/fonts/Roboto-Medium.ttf",
                                "app0:assets/fonts/Roboto-Bold.ttf"};
// sa0:data/font/pvf/ holds plain OpenType and TrueType files: Rodin (Japanese, regular and
// bold), DynaFont Hei (Chinese) and the Korean system font.
const char* const kSystem[6] = {"jpn0", "jpn2", "cn0", "cn0", "kr0", "kr2"};

int noto_face(Script s, int bold) { return F_NOTO + 2 * ((int)s - kNotoFirst) + (bold ? 1 : 0); }
}  // namespace

// Glyphs from every face are rendered on demand into one 8-bit atlas, so a line of text is one
// batch of quads whatever mix of scripts it holds.
struct FontEngine {
    struct Face {
        std::string path;
        bool memory = true;  // read into RAM (HarfBuzz needs that); the big system fonts are streamed
        float embolden = 0;  // synthetic bold, as a fraction of the pixel size
        bool tried = false;
        FT_Face ft = nullptr;
        std::string data;
        hb_font_t* hb = nullptr;
        int upem = 1000;
        int sizePx = 0;
    };
    struct Glyph {
        int16_t ax = 0, ay = 0, w = 0, h = 0, left = 0, top = 0;
        float adv = 0;
        bool placed = false;  // bitmap is in the atlas
    };

    FT_Library lib = nullptr;
    Face faces[F_COUNT];
    float ascEm = 0.928f, descEm = 0.244f;
    vita2d_texture* atlas = nullptr;
    uint8_t* pix = nullptr;
    unsigned stride = 0;
    int penX = 1, penY = 1, shelfH = 0;
    bool full = false;
    uint8_t gamma[256];
    std::unordered_map<uint32_t, Glyph> glyphs;  // face << 24 | px << 16 | glyph index
    std::unordered_map<uint32_t, uint32_t> picks;  // (cp, weight, japanese) -> (face + 1) << 16 | glyph index
    hb_buffer_t* buf = nullptr;

    bool init() {
        if (FT_Init_FreeType(&lib)) return false;
        for (int i = 0; i < 3; i++) faces[F_ROBOTO + i].path = kRoboto[i];
        for (int s = kNotoFirst; s <= kNotoLast; s++) {
            Script sc = (Script)s;
            Face& r = faces[noto_face(sc, 0)];
            Face& m = faces[noto_face(sc, 1)];
            r.path = str_printf("app0:assets/fonts/noto/NotoSans%s-Regular.ttf", noto_family(sc));
            if (noto_has_medium(sc)) {
                m.path = str_printf("app0:assets/fonts/noto/NotoSans%s-Medium.ttf", noto_family(sc));
            } else {
                m.path = r.path;
                m.embolden = 0.035f;
            }
        }
        for (int i = 0; i < 6; i++) {
            Face& f = faces[F_JPN + i];
            f.path = str_printf("sa0:data/font/pvf/%s.pvf", kSystem[i]);
            f.memory = false;
        }
        faces[F_CN + 1].embolden = 0.03f;
        for (int i = 0; i < 3; i++) {
            if (!face(F_ROBOTO + i)) return false;
        }
        FT_Face f = faces[F_ROBOTO].ft;
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
        buf = hb_buffer_create();
        return true;
    }

    void shutdown() {
        if (atlas) {
            vita2d_wait_rendering_done();
            vita2d_free_texture(atlas);
        }
        atlas = nullptr;
        if (buf) hb_buffer_destroy(buf);
        buf = nullptr;
        for (auto& f : faces) {
            if (f.hb) hb_font_destroy(f.hb);
            if (f.ft) FT_Done_Face(f.ft);
            f.hb = nullptr;
            f.ft = nullptr;
            std::string().swap(f.data);
            f.tried = false;
        }
        if (lib) FT_Done_FreeType(lib);
        lib = nullptr;
    }

    // The face, loading it the first time it is needed; nullptr if it can't be loaded.
    Face* face(int i) {
        if (i < 0 || i >= F_COUNT) return nullptr;
        Face& f = faces[i];
        if (!f.tried) {
            f.tried = true;
            bool ok;
            if (f.memory) {
                ok = read_file(f.path, f.data) && !FT_New_Memory_Face(lib, (const FT_Byte*)f.data.data(),
                                                                      (FT_Long)f.data.size(), 0, &f.ft);
            } else {
                ok = !FT_New_Face(lib, f.path.c_str(), 0, &f.ft);
            }
            if (!ok) {
                VT_LOG("font: cannot load %s", f.path.c_str());
                f.ft = nullptr;
                std::string().swap(f.data);
            } else {
                f.upem = f.ft->units_per_EM > 0 ? f.ft->units_per_EM : 1000;
                if (f.memory) {
                    hb_blob_t* blob = hb_blob_create(f.data.data(), (unsigned)f.data.size(), HB_MEMORY_MODE_READONLY,
                                                     nullptr, nullptr);
                    hb_face_t* hf = hb_face_create(blob, 0);
                    f.hb = hb_font_create(hf);
                    hb_font_set_scale(f.hb, f.upem, f.upem);
                    hb_face_destroy(hf);
                    hb_blob_destroy(blob);
                }
                if (i >= F_NOTO) VT_LOG("font: loaded %s", f.path.c_str());
            }
        }
        return f.ft ? &f : nullptr;
    }

    // The glyph for cp, or 0. HarfBuzz's cmap reader is used where there is one: the Vita build
    // of FreeType only exposes one cmap subtable of the Noto Indic fonts and misses their letters.
    unsigned index(int fi, uint32_t cp) {
        Face* f = face(fi);
        if (!f) return 0;
        if (f->hb) {
            hb_codepoint_t g = 0;
            return hb_font_get_nominal_glyph(f->hb, cp, &g) ? (unsigned)g : 0;
        }
        return FT_Get_Char_Index(f->ft, cp);
    }

    // The face that draws cp at weight w (Han characters prefer the Japanese font in Japanese
    // text, the Chinese one otherwise), and its glyph index. Face F_PGF when none has it.
    int pick(uint32_t cp, int w, bool jp, unsigned* gid) {
        uint32_t key = (cp & 0x1FFFFF) | ((uint32_t)w << 21) | ((uint32_t)jp << 23);
        auto it = picks.find(key);
        if (it != picks.end()) {
            *gid = it->second & 0xFFFF;
            return (int)(it->second >> 16) - 1;
        }
        int b = w > 0 ? 1 : 0;
        int c[3] = {-1, -1, -1};
        Script s = script_of(cp);
        switch (s) {
        case Script::Common:
            c[0] = F_ROBOTO + w, c[1] = F_JPN + b, c[2] = F_CN + b;
            break;
        case Script::Kana:
            c[0] = F_JPN + b, c[1] = F_CN + b;
            break;
        case Script::Han:
        case Script::CjkSymbol:
            c[0] = jp ? F_JPN + b : F_CN + b, c[1] = jp ? F_CN + b : F_JPN + b;
            break;
        case Script::Hangul:
            c[0] = F_KR + b, c[1] = F_CN + b;
            break;
        default:
            c[0] = noto_face(s, b), c[1] = F_ROBOTO + w, c[2] = F_JPN + b;
            break;
        }
        int r = F_PGF;
        unsigned g = 0;
        for (int f : c) {
            if (f >= 0 && (g = index(f, cp)) != 0) {
                r = f;
                break;
            }
        }
        if (r == F_PGF) g = 0;
        picks[key] = ((uint32_t)(r + 1) << 16) | (g & 0xFFFF);
        *gid = g;
        return r;
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

    // A glyph's bitmap in the atlas and its advance at px pixels.
    const Glyph* glyph(int fi, unsigned gid, int px) {
        uint32_t key = ((uint32_t)fi << 24) | ((uint32_t)px << 16) | (gid & 0xFFFF);
        auto it = glyphs.find(key);
        if (it != glyphs.end() && (it->second.placed || it->second.w == 0 || full)) return &it->second;
        Face* f = face(fi);
        if (!f) return nullptr;
        Glyph& g = glyphs[key];
        g = Glyph();
        if (f->sizePx != px) {
            FT_Set_Pixel_Sizes(f->ft, 0, px);
            f->sizePx = px;
        }
        if (FT_Load_Glyph(f->ft, gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP)) return &g;
        FT_GlyphSlot sl = f->ft->glyph;
        g.adv = sl->linearHoriAdvance / 65536.0f;
        if (f->embolden > 0 && sl->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Pos str = (FT_Pos)std::lround(px * f->embolden * 64);
            FT_Outline_EmboldenXY(&sl->outline, str, str);
            if (g.adv > 0) g.adv += px * f->embolden;
        }
        if (sl->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(sl, FT_RENDER_MODE_LIGHT)) return &g;
        const FT_Bitmap& bm = sl->bitmap;
        g.left = (int16_t)sl->bitmap_left;
        g.top = (int16_t)sl->bitmap_top;
        g.w = (int16_t)bm.width;
        g.h = (int16_t)bm.rows;
        if (g.w == 0 || g.h == 0) return &g;
        if (penX + g.w + 1 > kAtlasW) {
            penX = 1;
            penY += shelfH + 1;
            shelfH = 0;
        }
        if (penY + g.h + 1 > kAtlasH || g.w + 2 > kAtlasW) {
            full = true;  // drawn from the next frame on, after the atlas is recycled
            return &g;
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
        return &g;
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

// ---------------------------------------------------------------- shaped lines

// A line of complex text (right-to-left, Indic, Thai...) after bidi reordering and shaping:
// glyphs, emoji and PGF fallbacks in visual order, positioned from the line's start in pixels.
struct ShapedLine {
    enum Kind : uint8_t { GLYPH, EMOJI, PGF };
    struct Item {
        Kind kind;
        int16_t face;
        uint32_t id;  // glyph index, emoji pack index, or code point
        float x, y;   // y: offset above the baseline
    };
    std::vector<Item> items;
    float width = 0;
};

struct TextCache {
    std::unordered_map<std::string, ShapedLine> lines;
    std::unordered_map<std::string, std::string> ellipsized;
};

namespace {
int px_for(float scale) {
    int px = (int)std::lround(scale * kPxPerScale);
    return px < 6 ? 6 : (px > 200 ? 200 : px);
}

// Characters that join the font run they follow when that font has them: spaces, digits and
// punctuation, so "مرحبا، 2024" stays in one Arabic run.
bool inherits_font(uint32_t cp) {
    return (cp < 0x80 && !((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z')) || (cp >= 0xA0 && cp <= 0xBF) ||
           (cp >= 0x2000 && cp <= 0x206F) || cp == 0xFFFC;
}

// Base direction from the first strong character: true for right-to-left.
bool first_strong_rtl(const std::string& s) {
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return false;
            continue;
        }
        FriBidiCharType t = fribidi_get_bidi_type(utf8_decode(s, i));
        if (FRIBIDI_IS_STRONG(t) && FRIBIDI_IS_LETTER(t)) return FRIBIDI_IS_RTL(t) != 0;
    }
    return false;
}
}  // namespace

// Shapes one line: the bidi algorithm splits it into level runs, the runs are put in visual
// order (rule L2), each run is split where the font changes, and every piece is shaped with
// HarfBuzz in its run's direction.
static void shape_line(FontEngine& ft, EmojiAtlas* emoji, vita2d_pgf* pgf, const std::string& s, int w, int px,
                       ShapedLine& out) {
    out.items.clear();
    out.width = 0;
    bool jp = has_kana(s);
    std::vector<uint32_t> cps;
    std::vector<int> emo;  // emoji pack index, or -1
    cps.reserve(s.size());
    emo.reserve(s.size());
    for (size_t i = 0, next = 0; i < s.size(); i = next) {
        next = utf8_next(s, i);
        uint32_t cp = utf8_decode(s, i);
        if (cp == '\n' || cp == '\r') continue;
        if (cp == '\t' || cp == 0xA0) cp = ' ';
        if (emoji && cp >= '#') {
            size_t end = 0;
            bool textStyle = false;
            int idx = emoji->pack.match(s, i, &end, &textStyle);
            if (idx >= 0 && !textStyle) {
                cps.push_back(0xFFFC);  // object replacement: a neutral for the bidi algorithm
                emo.push_back(idx);
                next = end;
                continue;
            }
        }
        // Joiners stay for the shaper; other emoji components and unknown pictographs go.
        if (cp != 0x200C && cp != 0x200D && (is_emoji_component(cp) || is_pictograph(cp))) continue;
        cps.push_back(fold_styled(cp));
        emo.push_back(-1);
    }
    const int n = (int)cps.size();
    if (n == 0) return;

    std::vector<FriBidiCharType> types(n);
    std::vector<FriBidiBracketType> brackets(n);
    std::vector<FriBidiLevel> levels(n, 0);
    fribidi_get_bidi_types(cps.data(), n, types.data());
    fribidi_get_bracket_types(cps.data(), n, types.data(), brackets.data());
    FriBidiParType base = FRIBIDI_PAR_ON;
    if (!fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), n, &base, levels.data()))
        std::fill(levels.begin(), levels.end(), 0);
    // Rule L1: trailing whitespace goes back to the paragraph level.
    FriBidiLevel baseLevel = FRIBIDI_IS_RTL(base) ? 1 : 0;
    for (int k = n - 1; k >= 0 && (FRIBIDI_IS_EXPLICIT_OR_BN_OR_WS(types[k]) || FRIBIDI_IS_ISOLATE(types[k])); k--)
        levels[k] = baseLevel;

    struct Run {
        int start, end, level;
    };
    std::vector<Run> runs;
    for (int k = 0; k < n; k++) {
        if (runs.empty() || runs.back().level != levels[k]) runs.push_back({k, k + 1, levels[k]});
        else runs.back().end = k + 1;
    }
    int maxLevel = 0, minOdd = 255;
    for (auto& r : runs) {
        maxLevel = std::max(maxLevel, r.level);
        if (r.level & 1) minOdd = std::min(minOdd, r.level);
    }
    std::vector<int> order(runs.size());
    for (size_t k = 0; k < order.size(); k++) order[k] = (int)k;
    for (int lv = maxLevel; lv >= minOdd && lv > 0; lv--) {
        for (size_t a = 0; a < order.size();) {
            if (runs[order[a]].level < lv) {
                a++;
                continue;
            }
            size_t b = a;
            while (b < order.size() && runs[order[b]].level >= lv) b++;
            std::reverse(order.begin() + a, order.begin() + b);
            a = b;
        }
    }

    const float es = std::round(px * 1.12f), eAdv = es + std::round(px * 0.12f);
    const float pgfScale = px / 18.5f;
    float pen = 0;
    auto add = [&](ShapedLine::Kind k, int face, uint32_t id, float x, float y) {
        out.items.push_back({k, (int16_t)face, id, x, y});
    };
    struct Seg {
        int start, end, face;  // face -2: one emoji
    };
    std::vector<Seg> segs;
    for (int ri : order) {
        const Run& run = runs[ri];
        const bool rtl = run.level & 1;
        // Font runs, in logical order.
        segs.clear();
        for (int k = run.start; k < run.end; k++) {
            uint32_t c = cps[k];
            int f;
            unsigned gid = 0;
            int cur = segs.empty() ? -3 : segs.back().face;
            if (emo[k] >= 0) {
                f = -2;
                segs.push_back({k, k + 1, f});
                continue;
            }
            if (cur >= 0 && (is_invisible(c) || ((is_mark(c) || inherits_font(c)) && ft.index(cur, c))))
                f = cur;
            else if (is_invisible(c))
                f = F_ROBOTO + w;
            else
                f = ft.pick(c, w, jp, &gid);
            if (cur == f) segs.back().end = k + 1;
            else segs.push_back({k, k + 1, f});
        }
        if (rtl) std::reverse(segs.begin(), segs.end());
        for (const Seg& sg : segs) {
            if (sg.face == -2) {
                add(ShapedLine::EMOJI, 0, (uint32_t)emo[sg.start], pen, 0);
                pen += eAdv;
                continue;
            }
            FontEngine::Face* face = sg.face >= 0 ? ft.face(sg.face) : nullptr;
            if (face && face->hb) {
                hb_buffer_clear_contents(ft.buf);
                hb_buffer_add_utf32(ft.buf, cps.data(), n, (unsigned)sg.start, sg.end - sg.start);
                hb_buffer_set_direction(ft.buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
                hb_buffer_guess_segment_properties(ft.buf);
                int flags = HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES;
                if (sg.start == 0) flags |= HB_BUFFER_FLAG_BOT;
                if (sg.end == n) flags |= HB_BUFFER_FLAG_EOT;
                hb_buffer_set_flags(ft.buf, (hb_buffer_flags_t)flags);
                hb_shape(face->hb, ft.buf, nullptr, 0);
                unsigned gn = 0;
                hb_glyph_info_t* gi = hb_buffer_get_glyph_infos(ft.buf, &gn);
                hb_glyph_position_t* gp = hb_buffer_get_glyph_positions(ft.buf, &gn);
                const float k = (float)px / face->upem;
                const float extra = face->embolden * px;
                for (unsigned j = 0; j < gn; j++) {
                    add(ShapedLine::GLYPH, sg.face, gi[j].codepoint, pen + gp[j].x_offset * k, gp[j].y_offset * k);
                    pen += gp[j].x_advance * k + (gp[j].x_advance > 0 ? extra : 0);
                }
                continue;
            }
            // A streamed system font (no shaping needed for its scripts) or the PGF fallback:
            // one character at a time, reversed in right-to-left runs.
            for (int j = 0; j < sg.end - sg.start; j++) {
                int k = rtl ? sg.end - 1 - j : sg.start + j;
                uint32_t c = cps[k];
                if (is_invisible(c)) continue;
                if (face) {
                    unsigned gid = ft.index(sg.face, c);
                    const FontEngine::Glyph* g = ft.glyph(sg.face, gid, px);
                    add(ShapedLine::GLYPH, sg.face, gid, pen, 0);
                    if (g) pen += g->adv;
                } else if (pgf) {
                    std::string u;
                    utf8_append(u, c);
                    add(ShapedLine::PGF, F_PGF, c, pen, 0);
                    pen += vita2d_pgf_text_width(pgf, pgfScale, u.c_str());
                }
            }
        }
    }
    out.width = pen;
}

bool Gfx::init() {
    font_ = vita2d_load_default_pgf();
    ft_ = new FontEngine();
    if (!ft_->init()) {
        ft_->shutdown();
        delete ft_;
        ft_ = nullptr;
    }
    if (!font_ && !ft_) return false;
    cache_ = new TextCache();
    if (ft_) {
        emoji_ = new EmojiAtlas();
        if (!emoji_->init()) {
            emoji_->shutdown();
            delete emoji_;
            emoji_ = nullptr;
        }
        lineH_ = (ft_->ascEm + ft_->descEm) * kPxPerScale;
        ascent_ = ft_->ascEm * kPxPerScale;
        VT_LOG("gfx: roboto, line height %.1f, harfbuzz %s", lineH_, hb_version_string());
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
    delete cache_;
    cache_ = nullptr;
    if (font_) vita2d_free_pgf(font_);
    font_ = nullptr;
}

void Gfx::beginFrame() {
    if (ft_) ft_->recycle();
    if (emoji_) emoji_->beginFrame();
}

bool Gfx::rtl(const std::string& s) const { return first_strong_rtl(s); }

// Lays out one line. Draws it when `draw` is set; returns the advance width. Complex lines are
// shaped once and kept (`keep`), or shaped and dropped for the trial widths wrap() measures.
float Gfx::measure(const std::string& s, float scale, int weight, bool draw, float x, float y, uint32_t color,
                   bool keep) {
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
    const float iw = 1.0f / kAtlasW, ih = 1.0f / kAtlasH;
    // Emoji: a little larger than the letters, centred on the middle of the capitals.
    const float es = std::round(px * 1.12f), eAdv = es + std::round(px * 0.12f);
    const float eTop = std::round(baseline - px * 0.36f - es * 0.5f);
    const float ei = 1.0f / EmojiAtlas::kTex;
    vita2d_texture_vertex* v = nullptr;
    vita2d_texture_vertex* ev = nullptr;
    size_t nv = 0, nev = 0, cap = 0, ecap = 0;
    auto quad = [](vita2d_texture_vertex* out, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                   float v1) {
        vita2d_texture_vertex q[6] = {{x0, y0, 0.5f, u0, v0}, {x1, y0, 0.5f, u1, v0}, {x0, y1, 0.5f, u0, v1},
                                      {x0, y1, 0.5f, u0, v1}, {x1, y0, 0.5f, u1, v0}, {x1, y1, 0.5f, u1, v1}};
        memcpy(out, q, sizeof(q));
    };
    auto glyphQuad = [&](const FontEngine::Glyph* g, float gx, float gy) {
        if (!g || !g->placed || !v || nv + 6 > cap) return;
        float u0 = g->ax * iw, v0 = g->ay * ih, u1 = (g->ax + g->w) * iw, v1 = (g->ay + g->h) * ih;
        quad(v + nv, gx, gy, gx + g->w, gy + g->h, u0, v0, u1, v1);
        nv += 6;
    };
    auto emojiQuad = [&](int idx, float at) {
        const EmojiAtlas::Slot* sl = emoji_ ? emoji_->get(idx) : nullptr;
        if (!sl) return;
        if (!ev) {
            ecap = (s.size() / 2 + 1) * 6;
            ev = (vita2d_texture_vertex*)vita2d_pool_memalign((unsigned)(ecap * sizeof(vita2d_texture_vertex)),
                                                              sizeof(vita2d_texture_vertex));
        }
        if (!ev || nev + 6 > ecap) return;
        float gx = std::round(at + (eAdv - es) * 0.5f);
        int cx = (sl->cell % EmojiAtlas::kCols) * EmojiAtlas::kCell + 1;
        int cy = (sl->cell / EmojiAtlas::kCols) * EmojiAtlas::kCell + 1;
        quad(ev + nev, gx, eTop, gx + es, eTop + es, cx * ei, cy * ei, (cx + sl->size) * ei, (cy + sl->size) * ei);
        nev += 6;
    };
    auto finish = [&] {
        if (draw && v && nv) vita2d_draw_array_textured(ft_->atlas, SCE_GXM_PRIMITIVE_TRIANGLES, v, nv, color);
        if (draw && ev && nev) {
            vita2d_draw_array_textured(emoji_->tex, SCE_GXM_PRIMITIVE_TRIANGLES, ev, nev,
                                       with_alpha(0xFFFFFFFFu, (uint8_t)(color >> 24)));
        }
    };

    if (needs_shaping(s)) {
        ShapedLine tmp;
        const ShapedLine* L = &tmp;
        if (keep) {
            std::string key = s;
            key += '\x1f';
            key += (char)('0' + w);
            key += (char)px;
            auto it = cache_->lines.find(key);
            if (it == cache_->lines.end()) {
                if (cache_->lines.size() > 1500) cache_->lines.clear();
                it = cache_->lines.emplace(std::move(key), ShapedLine()).first;
                shape_line(*ft_, emoji_, font_, s, w, px, it->second);
            }
            L = &it->second;
        } else {
            shape_line(*ft_, emoji_, font_, s, w, px, tmp);
        }
        if (!draw) return L->width;
        cap = L->items.size() * 6;
        v = (vita2d_texture_vertex*)vita2d_pool_memalign((unsigned)(cap * sizeof(vita2d_texture_vertex)),
                                                         sizeof(vita2d_texture_vertex));
        for (const ShapedLine::Item& it : L->items) {
            if (it.kind == ShapedLine::GLYPH) {
                const FontEngine::Glyph* g = ft_->glyph(it.face, it.id, px);
                if (g) glyphQuad(g, std::round(pen + it.x) + g->left, baseline - std::round(it.y) - g->top);
            } else if (it.kind == ShapedLine::EMOJI) {
                emojiQuad((int)it.id, pen + it.x);
            } else if (font_) {
                std::string u;
                utf8_append(u, it.id);
                vita2d_pgf_draw_text(font_, (int)(pen + it.x), (int)baseline, color, pgfScale, u.c_str());
            }
        }
        finish();
        return L->width;
    }

    // One character at a time: Roboto with fallback fonts, emoji, then the PGF font.
    bool jp = has_kana(s);
    if (draw) {
        cap = s.size() * 6;
        v = (vita2d_texture_vertex*)vita2d_pool_memalign((unsigned)(cap * sizeof(vita2d_texture_vertex)),
                                                         sizeof(vita2d_texture_vertex));
    }
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
                if (draw) emojiQuad(idx, pen);
                pen += eAdv;
                next = end;
                continue;
            }
        }
        // Joiners, selectors, tones and pictographs the pack lacks: no font here draws them.
        if (is_emoji_component(cp) || is_pictograph(cp) || is_invisible(cp)) continue;
        bool bold = false;
        uint32_t c = fold_styled(cp, &bold);
        int cw = bold ? W_BOLD : w;
        unsigned gid = 0;
        int f = ft_->pick(c, cw, jp, &gid);
        if (f == F_PGF) {
            if (fallback.empty()) fallbackX = pen;
            fallback += s.substr(i, next - i);
            continue;
        }
        flush();
        const FontEngine::Glyph* g = ft_->glyph(f, gid, px);
        if (!g) continue;
        if (draw) glyphQuad(g, std::round(pen) + g->left, baseline - g->top);
        pen += g->adv;
    }
    flush();
    finish();
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

int Gfx::trialWidth(const std::string& s, float scale, int weight) {
    if (s.empty()) return 0;
    return (int)std::ceil(measure(s, scale, weight, false, 0, 0, 0, false));
}

namespace {
// Byte offsets inside s where a word may be split (between clusters).
std::vector<size_t> cluster_cuts(const std::string& s) {
    std::vector<size_t> cuts;
    uint32_t prev = 0;
    for (size_t i = 0; i < s.size(); i = utf8_next(s, i)) {
        uint32_t cp = utf8_decode(s, i);
        if (i > 0 && can_break_between(prev, cp)) cuts.push_back(i);
        prev = cp;
    }
    return cuts;
}
}  // namespace

std::string Gfx::ellipsize(const std::string& s, float maxWidth, float scale, int weight) {
    if (width(s, scale, weight) <= maxWidth) return s;
    std::string key = s;
    key += str_printf("\x1f%d\x1f%.2f\x1f%d", (int)maxWidth, scale, weight);
    auto it = cache_->ellipsized.find(key);
    if (it != cache_->ellipsized.end()) return it->second;
    if (cache_->ellipsized.size() > 2500) cache_->ellipsized.clear();
    // Binary search on cluster boundaries.
    std::vector<size_t> cuts = cluster_cuts(s);
    cuts.insert(cuts.begin(), 0);
    size_t lo = 0, hi = cuts.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (trialWidth(s.substr(0, cuts[mid]) + "...", scale, weight) <= maxWidth) lo = mid;
        else hi = mid;
    }
    std::string out = trim(s.substr(0, cuts[lo])) + "...";
    cache_->ellipsized.emplace(std::move(key), out);
    return out;
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
        const size_t paraFirst = lines.size();
        // Tokens: words separated by spaces; CJK characters are tokens of their own. A zero-width
        // space allows a break without adding a space.
        std::vector<std::pair<std::string, bool>> toks;  // token, preceded by a space
        std::string cur;
        bool curSpace = false, pendingSpace = false;
        for (size_t i = 0; i < para.size(); i = utf8_next(para, i)) {
            uint32_t cp = utf8_decode(para, i);
            size_t n = utf8_next(para, i) - i;
            if (cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x200B) {
                if (!cur.empty()) toks.push_back({cur, curSpace});
                cur.clear();
                if (cp != 0x200B) pendingSpace = true;
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
            if (trialWidth(cand, scale, weight) <= maxWidth) {
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
            // A word wider than the line: split it between clusters, as much per line as fits.
            std::string t = tk.first;
            while (trialWidth(t, scale, weight) > maxWidth) {
                std::vector<size_t> cuts = cluster_cuts(t);
                if (cuts.empty()) break;
                size_t lo = 0, hi = cuts.size();  // the first cut whose prefix doesn't fit
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    if (trialWidth(t.substr(0, cuts[mid]), scale, weight) <= maxWidth) lo = mid + 1;
                    else hi = mid;
                }
                size_t cut = cuts[lo > 0 ? lo - 1 : 0];
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
        // Each line is laid out on its own, so a line whose first strong character points the
        // other way than its paragraph gets a mark to keep the paragraph's direction.
        if (needs_shaping(para)) {
            bool paraRtl = first_strong_rtl(para);
            for (size_t k = paraFirst; k < lines.size(); k++) {
                if (first_strong_rtl(lines[k]) != paraRtl) lines[k].insert(0, paraRtl ? "\xE2\x80\x8F" : "\xE2\x80\x8E");
            }
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
    for (size_t i = 0; i < lines.size(); i++) textLine(x, y + i * lh, maxWidth, color, scale, lines[i], weight);
    return lines.size() * lh;
}

void Gfx::textLine(float x, float y, float maxWidth, uint32_t color, float scale, const std::string& s, int weight) {
    // Right-to-left lines line up on the right, like dir="auto" text on the web.
    float lx = rtl(s) ? x + maxWidth - width(s, scale, weight) : x;
    text(lx, y, color, scale, s, weight);
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
