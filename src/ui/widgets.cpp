#include "widgets.h"

#include <vita2d.h>

#include <algorithm>
#include <cmath>

#include "../core/util.h"
#include "app.h"

namespace vt {

namespace {

// Shared scroll physics for ListView and GridView: eased scrolling to a target, touch drag,
// fling with friction and rubber-band edges.
void settle_scroll(float& scroll, float& vel, float& target, bool touchActive, float maxScroll) {
    if (!touchActive) {
        if (std::fabs(vel) > 0.3f) {
            scroll -= vel;
            vel *= 0.94f;
        } else {
            vel = 0;
        }
        if (target >= 0) {
            scroll += (target - scroll) * 0.3f;
            if (std::fabs(target - scroll) < 0.5f) {
                scroll = target;
                target = -1;
            }
        }
    }
    if (scroll < 0) {
        if (!touchActive) scroll = scroll > -0.5f ? 0 : scroll * 0.6f;
    }
    if (scroll > maxScroll) {
        if (!touchActive) scroll = scroll - maxScroll < 0.5f ? maxScroll : maxScroll + (scroll - maxScroll) * 0.6f;
    }
}

void draw_scrollbar(const Rect& r, float scroll, float total) {
    if (total <= r.h + 1) return;
    float barH = std::max(24.0f, r.h * r.h / total);
    float barY = r.y + (r.h - barH) * std::max(0.0f, std::min(1.0f, scroll / (total - r.h)));
    g_gfx.roundRect(r.x + r.w - 5, barY + 2, 3, barH - 4, 1.5f, rgba(255, 255, 255, 40));
}

// Consumes a d-pad direction only when the view moved.
bool take(Input& in, uint32_t b) {
    in.consume(b);
    return true;
}

}  // namespace

// ---------------------------------------------------------------- ListView

float ListView::heightOf(int i) const { return (int)heights.size() == count ? heights[i] : rowH; }

float ListView::offsetOf(int i) const {
    float b = hasBand() && i >= bandAt ? bandH : 0;
    if ((int)heights.size() != count) return i * rowH + b;
    float o = b;
    for (int k = 0; k < i && k < count; k++) o += heights[k];
    return o;
}

float ListView::total() const { return offsetOf(count); }

int ListView::indexAt(float y) const {
    if (hasBand()) {
        float bt = bandTop();
        if (y >= bt && y < bt + bandH) return -1;  // the band
        if (y >= bt + bandH) y -= bandH;
    }
    if ((int)heights.size() != count) return rowH > 0 ? (int)(y / rowH) : 0;
    float o = 0;
    for (int k = 0; k < count; k++) {
        if (y < o + heights[k]) return k;
        o += heights[k];
    }
    return count;
}

void ListView::ensureVisible(const Rect& r, bool snap) {
    if (count == 0) return;
    float top = offsetOf(sel), h = heightOf(sel);
    if (bandSel && hasBand()) {
        top = bandTop();
        h = bandH;
    }
    float t = target_ >= 0 ? target_ : scroll_;
    if (top < t) t = top;
    if (top + h > t + r.h) t = top + h - r.h;
    float maxScroll = std::max(0.0f, total() - r.h);
    t = std::max(0.0f, std::min(t, maxScroll));
    if (snap) scroll_ = t;
    target_ = t;
}

int ListView::update(Input& in, const Rect& r) {
    if (count <= 0) {
        sel = 0;
        scroll_ = 0;
        return -1;
    }
    sel = std::max(0, std::min(sel, count - 1));
    int activated = -1;
    float maxScroll = std::max(0.0f, total() - r.h);
    if (!hasBand()) bandSel = false;

    if (focused) {
        int before = sel;
        bool bandBefore = bandSel;
        if (bandSel) {
            if (in.repeat(BTN_DOWN) && bandAt < count && take(in, BTN_DOWN)) {
                bandSel = false;
                sel = bandAt;
            } else if (in.repeat(BTN_UP) && bandAt > 0 && take(in, BTN_UP)) {
                bandSel = false;
                sel = bandAt - 1;
            }
        } else {
            if (in.repeat(BTN_DOWN)) {
                if (hasBand() && sel + 1 == bandAt && take(in, BTN_DOWN)) bandSel = true;
                else if (sel + 1 < count && take(in, BTN_DOWN)) sel++;
            }
            if (in.repeat(BTN_UP)) {
                if (hasBand() && sel == bandAt && take(in, BTN_UP)) bandSel = true;
                else if (sel > 0 && take(in, BTN_UP)) sel--;
            }
            if (in.pressed(BTN_CONFIRM)) {
                activated = sel;
                in.consume(BTN_CONFIRM);
            }
        }
        if (sel != before || bandSel != bandBefore) {
            vel_ = 0;
            ensureVisible(r, false);
        }
    }

    // Touch: drag to scroll, fling, tap to activate.
    if (in.touchBegan && r.contains(in.tx, in.ty)) {
        touchActive_ = true;
        vel_ = 0;
        target_ = -1;
    }
    if (touchActive_) {
        if (in.touching && in.dragging) {
            scroll_ -= in.dy;
            vel_ = in.velocityY;
        }
        if (in.touchEnded) {
            touchActive_ = false;
            if (scroll_ < -PULL_TO_REFRESH) pulled_ = true;
            if (in.tapped && r.contains(in.tapX, in.tapY)) {
                int idx = indexAt(in.tapY - r.y + scroll_);
                if (idx >= 0 && idx < count) {
                    sel = idx;
                    bandSel = false;
                    activated = idx;
                    in.consumeTap();
                }
            }
        }
    }
    if (!touchActive_ && std::fabs(vel_) > 0.3f) {
        // Keep the selection on screen while flinging so the d-pad continues from there.
        int top = indexAt(scroll_ + 1), bottom = indexAt(scroll_ + r.h - 1);
        if (top >= 0 && sel < top) sel = std::min(top + 1, count - 1), bandSel = false;
        if (bottom >= 0 && sel > bottom) sel = std::max(bottom - 1, 0), bandSel = false;
    }
    settle_scroll(scroll_, vel_, target_, touchActive_, maxScroll);
    return activated;
}

void ListView::draw(const Rect& r, const std::function<void(int, const Rect&, bool)>& drawRow,
                    const std::function<void(float y)>& band) {
    if (count <= 0) return;
    g_gfx.clip(r);
    int at = indexAt(std::max(0.0f, scroll_));
    if (at < 0) at = bandAt;
    int first = std::max(0, at - 1);
    for (int i = first; i < count; i++) {
        float y = r.y + offsetOf(i) - scroll_;
        if (y >= r.y + r.h) break;
        float h = heightOf(i);
        if (y + h > r.y) drawRow(i, Rect{r.x, y, r.w, h}, focused && !bandSel && i == sel);
    }
    if (band && hasBand()) {
        float by = r.y + bandTop() - scroll_;
        if (by < r.y + r.h && by + bandH > r.y) band(by);
    }
    draw_scrollbar(r, scroll_, total());
    g_gfx.unclip();
}

// ---------------------------------------------------------------- GridView

void GridView::layout(float width, int columns, float textH) {
    cols = std::max(1, columns);
    cellW = std::floor((width - 2 * padX - (cols - 1) * gapX) / cols);
    cellH = std::floor(cellW * 9.0f / 16.0f) + textH;
}

float GridView::total() const {
    return headerH + padTop + rows() * (cellH + gapY) + (hasBand() ? bandH : 0) + 8;
}

void GridView::ensureVisible(const Rect& r, bool snap) {
    if (count == 0) return;
    int row = sel / cols;
    float top = row == 0 && !(hasBand() && bandRow == 0) ? 0 : rowTop(row) - padTop;  // row 0 also reveals the header
    float bottom = rowTop(row) + cellH + gapY;
    if (bandSel && hasBand()) {
        top = bandRow == 0 ? 0 : bandTop() - padTop;
        bottom = bandTop() + bandH;
    }
    float t = target_ >= 0 ? target_ : scroll_;
    if (top < t) t = top;
    if (bottom > t + r.h) t = bottom - r.h;
    float maxScroll = std::max(0.0f, total() - r.h);
    t = std::max(0.0f, std::min(t, maxScroll));
    if (snap) scroll_ = t;
    target_ = t;
}

int GridView::indexAt(const Rect& r, float x, float y) const {
    float cy = y - r.y + scroll_ - headerH - padTop;
    float cx = x - r.x - padX;
    if (hasBand()) {
        float bt = bandTop() - headerH - padTop;
        if (cy >= bt && cy < bt + bandH) return -1;  // the band
        if (cy >= bt + bandH) cy -= bandH;
    }
    if (cy < 0 || cx < 0) return -1;
    int row = (int)(cy / (cellH + gapY)), c = (int)(cx / (cellW + gapX));
    if (c >= cols) return -1;
    if (cy - row * (cellH + gapY) > cellH || cx - c * (cellW + gapX) > cellW) return -1;  // in a gap
    int idx = row * cols + c;
    return idx < count ? idx : -1;
}

bool GridView::headerTapped(const Input& in, const Rect& r, float& hx, float& hy) const {
    if (!in.tapped || headerH <= 0 || !r.contains(in.tapX, in.tapY)) return false;
    float y = in.tapY - r.y + scroll_;
    if (y >= headerH) return false;
    hx = in.tapX - r.x;
    hy = y;
    return true;
}

int GridView::update(Input& in, const Rect& r) {
    float maxScroll = std::max(0.0f, total() - r.h);
    if (count <= 0) {
        sel = 0;
        settle_scroll(scroll_, vel_, target_, touchActive_, maxScroll);
        return -1;
    }
    sel = std::max(0, std::min(sel, count - 1));
    int activated = -1;
    if (!hasBand()) bandSel = false;
    if (focused) {
        int before = sel;
        bool bandBefore = bandSel;
        int col = sel % cols, row = sel / cols, lastRow = (count - 1) / cols;
        if (bandSel) {
            // The owner moves within the band; Up / Down leave it, back to the same column.
            if (in.repeat(BTN_DOWN) && bandRow <= lastRow && take(in, BTN_DOWN)) {
                bandSel = false;
                sel = std::min(count - 1, bandRow * cols + col);
            } else if (in.repeat(BTN_UP) && bandRow > 0 && take(in, BTN_UP)) {
                bandSel = false;
                sel = std::min(count - 1, (bandRow - 1) * cols + col);
            }
        } else {
            if (in.repeat(BTN_RIGHT) && col + 1 < cols && sel + 1 < count && take(in, BTN_RIGHT)) sel++;
            if (in.repeat(BTN_LEFT) && col > 0 && take(in, BTN_LEFT)) sel--;
            if (in.repeat(BTN_DOWN)) {
                if (hasBand() && row + 1 == bandRow && take(in, BTN_DOWN)) bandSel = true;
                else if (row < lastRow && take(in, BTN_DOWN)) sel = std::min(count - 1, sel + cols);
            }
            if (in.repeat(BTN_UP)) {
                if (hasBand() && row == bandRow && take(in, BTN_UP)) bandSel = true;
                else if (row > 0 && take(in, BTN_UP)) sel -= cols;
            }
            if (in.pressed(BTN_CONFIRM)) {
                activated = sel;
                in.consume(BTN_CONFIRM);
            }
        }
        if (sel != before || bandSel != bandBefore) {
            vel_ = 0;
            ensureVisible(r, false);
        }
    }
    if (in.touchBegan && r.contains(in.tx, in.ty)) {
        touchActive_ = true;
        vel_ = 0;
        target_ = -1;
    }
    if (touchActive_) {
        if (in.touching && in.dragging) {
            scroll_ -= in.dy;
            vel_ = in.velocityY;
        }
        if (in.touchEnded) {
            touchActive_ = false;
            if (scroll_ < -PULL_TO_REFRESH) pulled_ = true;
            if (in.tapped && r.contains(in.tapX, in.tapY)) {
                int idx = indexAt(r, in.tapX, in.tapY);
                if (idx >= 0) {
                    sel = idx;
                    bandSel = false;
                    activated = idx;
                    in.consumeTap();
                }
            }
        }
    }
    if (!touchActive_ && std::fabs(vel_) > 0.3f && !bandSel) {
        float b = hasBand() && scroll_ > bandTop() ? bandH : 0;
        int rowsAbove = (int)std::max(0.0f, (scroll_ - b - headerH - padTop) / (cellH + gapY) + 0.6f);
        int rowsBelow = (int)((scroll_ - b + r.h - headerH - padTop) / (cellH + gapY)) - 1;
        int row = sel / cols;
        if (row < rowsAbove) sel = std::min(count - 1, rowsAbove * cols + sel % cols);
        if (row > rowsBelow && rowsBelow >= 0) sel = std::max(0, rowsBelow * cols + sel % cols);
    }
    settle_scroll(scroll_, vel_, target_, touchActive_, maxScroll);
    return activated;
}

void GridView::draw(const Rect& r, const std::function<void(int, const Rect&, bool)>& cell,
                    const std::function<void(float y)>& header, const std::function<void(float y)>& band) {
    g_gfx.clip(r);
    if (header && headerH > 0 && scroll_ < headerH) header(r.y - scroll_);
    if (count > 0) {
        int nrows = rows();
        float b = hasBand() && scroll_ > bandTop() ? bandH : 0;
        int firstRow = std::max(0, (int)((scroll_ - b - headerH - padTop) / (cellH + gapY)) - 1);
        for (int row = firstRow; row < nrows; row++) {
            float y = r.y + rowTop(row) - scroll_;
            if (y > r.y + r.h) break;
            if (y + cellH + 6 < r.y) continue;
            for (int c = 0; c < cols; c++) {
                int i = row * cols + c;
                if (i >= count) break;
                cell(i, Rect{r.x + padX + c * (cellW + gapX), y, cellW, cellH}, focused && !bandSel && i == sel);
            }
        }
        if (band && hasBand()) {
            float by = r.y + bandTop() - scroll_;
            if (by < r.y + r.h && by + bandH > r.y) band(by);
        }
    }
    draw_scrollbar(r, scroll_, total());
    g_gfx.unclip();
}

// ---------------------------------------------------------------- items

std::string meta_line(const Item& it) {
    if (it.type == ItemType::Channel) return it.extra;
    if (it.type == ItemType::Playlist) return it.extra.empty() ? "Playlist" : "Playlist" G_DOT + it.extra;
    std::string pub = !it.published.empty() ? it.published : (it.time > 0 ? format_age(it.time, unix_now()) : "");
    if (it.live) return it.views.empty() ? "Live" : it.views;
    if (it.views.empty()) return pub;
    if (pub.empty()) return it.views;
    return it.views + G_DOT + pub;
}

static void draw_badge(float rx, float by, const std::string& label, uint32_t fill, Icon icon, bool withIcon) {
    const float s = 0.6f;
    float tw = (float)g_gfx.width(label, s, true);
    float w = tw + 10 + (withIcon ? 16 : 0), h = 17;
    float x = rx - w, y = by - h;
    g_gfx.roundRect(x, y, w, h, 4, fill);
    float tx = x + 5;
    if (withIcon) {
        draw_icon(icon, tx + 6, y + h / 2, 12, col::text, fill);
        tx += 16;
    }
    g_gfx.textBold(tx, y + (h - g_gfx.lineHeight(s)) / 2, col::text, s, label);
}

void draw_thumb(const Item& it, float x, float y, float w, float h, float radius, uint32_t bg, float progress,
                bool badges) {
    if (it.type == ItemType::Playlist) {
        // Stacked look: two thinner layers peeking out above the cover.
        g_gfx.roundRect(x + w * 0.08f, y - 7, w * 0.84f, 10, 4, rgba(70, 70, 70));
        g_gfx.roundRect(x + w * 0.04f, y - 4, w * 0.92f, 10, 4, rgba(100, 100, 100));
    }
    g_gfx.roundRect(x, y, w, h, radius, col::thumbBg);
    std::string url = it.thumbUrl();
    if (!url.empty())
        if (vita2d_texture* tex = g_app->thumbs.get(url)) g_gfx.imageCover(tex, x, y, w, h);
    if (badges) {
        if (it.type == ItemType::Video && it.live) draw_badge(x + w - 6, y + h - 6, "LIVE", rgba(204, 0, 0), Icon::Play, false);
        else if (it.type == ItemType::Video && !it.duration.empty()) draw_badge(x + w - 6, y + h - 6, it.duration, col::badge, Icon::Play, false);
        else if (it.type == ItemType::Playlist) {
            std::string n = it.extra.empty() ? "Playlist" : it.extra;
            draw_badge(x + w - 6, y + h - 6, n, col::badge, Icon::Playlist, true);
        }
    }
    if (progress >= 0) {
        float p = std::min(1.0f, std::max(0.03f, progress));
        g_gfx.rect(x, y + h - 4, w, 4, rgba(255, 255, 255, 70));
        g_gfx.rect(x, y + h - 4, w * p, 4, col::red);
    }
    g_gfx.cornerMask(x, y, w, h, radius, bg);
}

static uint32_t avatar_color(const std::string& name) {
    static const uint32_t palette[] = {rgba(239, 108, 0),  rgba(0, 137, 123),  rgba(94, 53, 177), rgba(216, 27, 96),
                                       rgba(30, 136, 229), rgba(67, 160, 71),  rgba(244, 81, 30), rgba(0, 151, 167),
                                       rgba(124, 77, 255), rgba(192, 202, 51)};
    uint32_t h = 2166136261u;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

void draw_avatar(const std::string& url, const std::string& name, float cx, float cy, float r, uint32_t bg) {
    vita2d_texture* tex = url.empty() ? nullptr : g_app->thumbs.get(url);
    if (tex) {
        g_gfx.circle(cx, cy, r, col::thumbBg);
        g_gfx.imageCover(tex, cx - r, cy - r, 2 * r, 2 * r);
        g_gfx.cornerMask(cx - r, cy - r, 2 * r, 2 * r, r, bg);
        return;
    }
    g_gfx.circle(cx, cy, r, name.empty() ? col::thumbBg : avatar_color(name));
    if (name.empty()) return;
    size_t start = name[0] == '@' && name.size() > 1 ? 1 : 0;  // handles: use the first letter
    std::string initial = name.substr(start, utf8_next(name, start) - start);
    if (initial.size() == 1 && initial[0] >= 'a' && initial[0] <= 'z') initial[0] = (char)(initial[0] - 32);
    float s = r / 16.0f;
    g_gfx.textCentered(cx, cy - g_gfx.lineHeight(s) / 2, col::text, s, initial, true);
}

void draw_focus_ring(float x, float y, float w, float h, float radius) {
    g_gfx.roundFrame(x - 4, y - 4, w + 8, h + 8, radius + 4, 3, col::focus);
}

void draw_progress_bar(float x, float y, float w, float frac) {
    g_gfx.rect(x, y, w, 4, rgba(255, 255, 255, 70));
    g_gfx.rect(x, y, w * std::max(0.0f, std::min(1.0f, frac)), 4, col::red);
}

float draw_channel_name(float x, float y, uint32_t color, float scale, const std::string& name, bool verified,
                        float maxW, uint32_t bg) {
    float lh = g_gfx.lineHeight(scale);
    float isz = std::round(lh * 0.6f);
    std::string s = g_gfx.ellipsize(name, verified ? maxW - isz - 5 : maxW, scale);
    g_gfx.text(x, y, color, scale, s);
    float w = (float)g_gfx.width(s, scale);
    if (verified) {
        draw_icon(Icon::Verified, x + w + 4 + isz / 2, y + lh * 0.52f, isz, color, bg);
        w += isz + 5;
    }
    return w;
}

float draw_meta_badges(float x, float y, const std::vector<std::string>& badges, float maxW) {
    const float s = 0.56f, h = 18;
    float x0 = x;
    for (const std::string& b : badges) {
        float w = (float)g_gfx.width(b, s, W_MEDIUM) + 10;
        if (x + w > x0 + maxW) break;
        g_gfx.roundRect(x, y, w, h, 2, rgba(255, 255, 255, 26));
        g_gfx.text(x + 5, y + (h - g_gfx.lineHeight(s)) / 2, col::text2, s, b, W_MEDIUM);
        x += w + 4;
    }
    return x - x0;
}

void draw_shorts_card(const Item& it, const Rect& c, bool focused, uint32_t bg) {
    float th = std::floor(c.w * 16.0f / 9.0f);
    g_gfx.roundRect(c.x, c.y, c.w, th, 8, col::thumbBg);
    std::string url = it.thumbUrl();
    if (!url.empty())
        if (vita2d_texture* tex = g_app->thumbs.get(url)) g_gfx.imageCover(tex, c.x, c.y, c.w, th);
    g_gfx.cornerMask(c.x, c.y, c.w, th, 8, bg);
    if (focused) draw_focus_ring(c.x, c.y, c.w, th, 8);
    float y = c.y + th + 8;
    float w = c.w - (focused ? 18 : 2);
    if (focused) draw_icon(Icon::More, c.x + c.w - 6, y + 9, 15, col::text);
    y += g_gfx.paragraph(c.x, y, w, col::text, 0.72f, it.title, 2, 1.0f, W_MEDIUM) + 2;
    if (!it.views.empty()) g_gfx.text(c.x, y, col::text2, 0.64f, g_gfx.ellipsize(it.views, c.w, 0.64f));
}

void draw_video_card(const Item& it, const Rect& c, bool focused, bool showAvatar, float progress) {
    float th = std::floor(c.w * 9.0f / 16.0f);
    if (it.type == ItemType::Channel) {
        // Channel card: a large round avatar in place of the thumbnail.
        float r = th * 0.42f;
        draw_avatar(it.thumb, it.title, c.x + c.w / 2, c.y + th / 2, r);
        if (focused) g_gfx.ring(c.x + c.w / 2, c.y + th / 2, r + 5, 3, col::focus);
        float y = c.y + th + 10;
        g_gfx.textCentered(c.x + c.w / 2, y, col::text, 0.8f, g_gfx.ellipsize(it.title, c.w - 8, 0.8f), true);
        if (!it.extra.empty())
            g_gfx.textCentered(c.x + c.w / 2, y + 20, col::text2, 0.68f, g_gfx.ellipsize(it.extra, c.w - 8, 0.68f));
        return;
    }
    draw_thumb(it, c.x, c.y, c.w, th, 10, col::bg, progress);
    if (focused) draw_focus_ring(c.x, c.y, c.w, th, 10);
    float y = c.y + th + 10;
    float x = c.x;
    if (showAvatar) {
        draw_avatar(it.avatar, it.channel, c.x + 17, y + 17, 17);
        x += 46;
    }
    float w = c.x + c.w - x - (focused ? 22 : 4);
    if (focused) draw_icon(Icon::More, c.x + c.w - 8, y + 9, 16, col::text);  // □ opens the options menu
    y += g_gfx.paragraph(x, y, w, col::text, 0.78f, it.title, 2, 1.0f, true) + 3;
    float s = 0.66f, lh = g_gfx.lineHeight(s);
    if (!it.channel.empty() && it.type != ItemType::Channel) {
        draw_channel_name(x, y, col::text2, s, it.channel, it.verified, w);
        y += lh;
    }
    std::string meta = meta_line(it);
    if (!meta.empty()) g_gfx.text(x, y, col::text2, s, g_gfx.ellipsize(meta, w, s));
}

void draw_result_row(const Item& it, const Rect& r, bool focused, float progress) {
    uint32_t bg = focused ? col::rowHi : col::bg;
    if (focused) g_gfx.roundRect(r.x + 8, r.y + 3, r.w - 20, r.h - 6, 12, bg);
    float th = r.h - 22, tw = std::floor(th * 16.0f / 9.0f);
    float tx = r.x + 18, ty = r.y + 11;
    float x = tx + tw + 16, w = r.x + r.w - x - 24;
    if (it.type == ItemType::Channel) {
        float rr = th * 0.46f;
        draw_avatar(it.thumb, it.title, tx + tw / 2, ty + th / 2, rr, bg);
        float y = ty + th / 2 - 30;
        g_gfx.textBold(x, y, col::text, 0.9f, g_gfx.ellipsize(it.title, w, 0.9f));
        y += 26;
        std::string sub = it.extra.empty() ? "Channel" : it.extra;
        g_gfx.text(x, y, col::text2, 0.68f, g_gfx.ellipsize(sub, w, 0.68f));
        bool subd = g_app->store.isSubscribed(it.id);
        draw_pill(x, y + 24, subd ? "Subscribed" : "Subscribe", subd ? PillStyle::Secondary : PillStyle::Primary, false,
                  false, Icon::Play, 30);
        return;
    }
    draw_thumb(it, tx, ty, tw, th, 10, bg, progress);
    if (focused) draw_icon(Icon::More, r.x + r.w - 22, ty + 10, 16, col::text);  // □ opens the options menu
    // Search-result layout: title (regular weight), stats, channel with avatar, snippet, badges.
    float y = ty - 2;
    y += g_gfx.paragraph(x, y, w - 12, col::text, 0.9f, it.title, 2, 1.0f, W_REGULAR) + 2;
    float s = 0.64f, lh = g_gfx.lineHeight(s);
    float bottom = ty + th;
    std::string meta = meta_line(it);
    if (it.type == ItemType::Video && !meta.empty()) {
        g_gfx.text(x, y, col::text2, s, g_gfx.ellipsize(meta, w, s));
        y += lh + 7;
    }
    if (!it.channel.empty()) {
        draw_avatar(it.avatar, it.channel, x + 11, y + 10, 11, bg);
        draw_channel_name(x + 30, y + 3, col::text2, s, it.channel, it.verified, w - 30, bg);
        y += 29;
    }
    if (it.type == ItemType::Playlist) {
        g_gfx.text(x, y, col::text2, s, g_gfx.ellipsize(meta, w, s));
        y += lh + 4;
        g_gfx.text(x, y, col::text2, s, "View full playlist", W_MEDIUM);
        return;
    }
    bool badges = !it.badges.empty() || it.live;
    float room = bottom - y - (badges ? 22 : 0);
    int lines = (int)(room / lh);
    if (!it.snippet.empty() && lines > 0) {
        y += g_gfx.paragraph(x, y, w, col::text2, s, it.snippet, std::min(2, lines), 1.0f) + 5;
    }
    if (badges && y + 18 <= bottom + 4) {
        float bx = x;
        if (it.live) {
            const float bs = 0.56f;
            float bw = (float)g_gfx.width("LIVE", bs, W_MEDIUM) + 26;
            g_gfx.roundRect(bx, y, bw, 18, 2, col::red);
            g_gfx.circle(bx + 9, y + 9, 3, col::text);
            g_gfx.text(bx + 16, y + (18 - g_gfx.lineHeight(bs)) / 2, col::text, bs, "LIVE", W_MEDIUM);
            bx += bw + 4;
        }
        std::vector<std::string> rest;
        for (const std::string& b : it.badges)
            if (!(it.live && (b == "LIVE" || b == "Live"))) rest.push_back(b);  // already shown in red
        draw_meta_badges(bx, y, rest, x + w - bx);
    }
}

void draw_compact_row(const Item& it, const Rect& r, bool focused, int index, bool current) {
    uint32_t bg = focused ? col::rowHi : current ? col::rowCur : col::bg;
    if (focused || current) g_gfx.roundRect(r.x + 4, r.y + 2, r.w - 12, r.h - 4, 10, bg);
    float x = r.x + 10;
    if (index >= 0) {
        float cx = x + 11, cy = r.y + r.h / 2;
        if (current) draw_icon(Icon::Play, cx, cy, 12, col::text2, bg);
        else g_gfx.textCentered(cx, cy - g_gfx.lineHeight(0.62f) / 2, col::text2, 0.62f, str_printf("%d", index + 1));
        x += 24;
    }
    float th = r.h - 14, tw = std::floor(th * 16.0f / 9.0f);
    float ty = r.y + 7;
    if (it.type == ItemType::Channel) {
        draw_avatar(it.thumb, it.title, x + tw / 2, ty + th / 2, th / 2, bg);
    } else {
        float prog = it.type == ItemType::Video ? g_app->store.watchedFraction(it.id) : -1.0f;
        draw_thumb(it, x, ty, tw, th, 8, bg, prog);
    }
    float tx = x + tw + 10, w = r.x + r.w - tx - 14;
    float y = ty - 1;
    y += g_gfx.paragraph(tx, y, w, col::text, 0.68f, it.title, 2, 1.0f, true) + 2;
    float s = 0.6f, lh = g_gfx.lineHeight(s);
    if (!it.channel.empty()) {
        draw_channel_name(tx, y, col::text2, s, it.channel, it.verified, w, bg);
        y += lh;
    }
    std::string meta = meta_line(it);
    if (!meta.empty() && y + lh <= r.y + r.h - 4) g_gfx.text(tx, y, col::text2, s, g_gfx.ellipsize(meta, w, s));
}

// ---------------------------------------------------------------- controls

static constexpr float kChipH = 32;

float chip_width(const std::string& s, float scale) { return (float)g_gfx.width(s, scale, true) + 26 * scale / 0.72f; }

void draw_chips(const std::vector<std::string>& names, int selected, int focusIdx, float x, float y, float scale,
                float gap) {
    const float kChipScale = scale, kChipGap = gap;
    for (size_t i = 0; i < names.size(); i++) {
        float w = chip_width(names[i], scale);
        bool on = (int)i == selected;
        bool foc = (int)i == focusIdx;
        g_gfx.roundRect(x, y, w, kChipH, 8, on ? col::text : foc ? col::raisedHi : col::raised);
        g_gfx.textCentered(x + w / 2, y + (kChipH - g_gfx.lineHeight(kChipScale)) / 2, on ? col::onLight : col::text,
                           kChipScale, names[i], true);
        if (foc) g_gfx.roundFrame(x - 3, y - 3, w + 6, kChipH + 6, 11, 2.5f, col::focus);
        x += w + kChipGap;
    }
}

int chips_hit(const Input& in, const std::vector<std::string>& names, float x, float y, float scale, float gap) {
    if (!in.tapped) return -1;
    for (size_t i = 0; i < names.size(); i++) {
        float w = chip_width(names[i], scale);
        if (in.tapIn(x, y - 4, w, kChipH + 8)) return (int)i;
        x += w + gap;
    }
    return -1;
}

static float pill_scale(float h) { return h >= 34 ? 0.74f : 0.68f; }

float pill_width(const std::string& label, bool withIcon, float h) {
    float w = (float)g_gfx.width(label, pill_scale(h), true) + h * 0.9f;
    if (withIcon) w += label.empty() ? 0 : h * 0.62f;
    if (label.empty()) w = h;
    return w;
}

float draw_pill(float x, float y, const std::string& label, PillStyle style, bool focused, bool withIcon, Icon icon,
                float h) {
    float w = pill_width(label, withIcon, h);
    float s = pill_scale(h);
    uint32_t fill = style == PillStyle::Primary ? col::text : focused ? col::raisedHi : col::raised;
    uint32_t fg = style == PillStyle::Primary ? col::onLight : col::text;
    g_gfx.roundRect(x, y, w, h, h / 2, fill);
    float tx = x + h * 0.45f;
    if (withIcon) {
        float isz = h * 0.5f;
        if (label.empty()) {
            draw_icon(icon, x + w / 2, y + h / 2, isz, fg, fill);
        } else {
            draw_icon(icon, tx + isz / 2 - 2, y + h / 2, isz, fg, fill);
            tx += h * 0.62f;
        }
    }
    if (!label.empty()) g_gfx.textBold(tx, y + (h - g_gfx.lineHeight(s)) / 2, fg, s, label);
    if (focused) g_gfx.roundFrame(x - 4, y - 4, w + 8, h + 8, h / 2 + 4, 2.5f, col::focus);
    return w;
}

void draw_switch(float x, float y, bool on) {
    // x, y: top-left of a 40 x 24 box.
    g_gfx.roundRect(x + 2, y + 5, 36, 14, 7, on ? rgba(38, 99, 153) : rgba(113, 113, 113));
    g_gfx.circle(on ? x + 30 : x + 10, y + 12, 10, on ? col::blue : rgba(204, 204, 204));
}

Rect draw_empty_state(const Rect& r, Icon icon, const std::string& title, const std::string& body,
                      const std::string& button, bool buttonFocused) {
    const auto& lines = g_gfx.wrap(body, std::min(520.0f, r.w - 60), 0.72f, 5);
    float lh = g_gfx.lineHeight(0.72f);
    float h = 96 + 30 + (body.empty() ? 0 : lines.size() * lh + 8) + (button.empty() ? 0 : 56);
    float cx = r.x + r.w / 2;
    float y = r.y + std::max(10.0f, (r.h - h) / 2);
    g_gfx.circle(cx, y + 42, 42, col::raised);
    draw_icon(icon, cx, y + 42, 40, col::text2, col::raised);
    y += 100;
    g_gfx.textCentered(cx, y, col::text, 0.92f, g_gfx.ellipsize(title, r.w - 40, 0.92f), true);
    y += 30;
    if (!body.empty()) {
        for (size_t i = 0; i < lines.size(); i++) g_gfx.textCentered(cx, y + i * lh, col::text2, 0.72f, lines[i]);
        y += lines.size() * lh + 12;
    }
    if (button.empty()) return Rect{};
    float bw = pill_width(button);
    draw_pill(cx - bw / 2, y, button, PillStyle::Secondary, buttonFocused);
    return Rect{cx - bw / 2, y, bw, 36};
}

void draw_loading(const Rect& r) { g_gfx.spinner(r.x + r.w / 2, r.y + r.h / 2, 18, col::text2); }

// Grey placeholder cards shown while the first page loads.
static void draw_skeleton_grid(const Rect& r, const GridView& g) {
    float a = 0.55f + 0.25f * std::sin((float)g_gfx.time * 3.0f);
    uint32_t c1 = rgba(39, 39, 39, (uint8_t)(255 * a)), c2 = rgba(39, 39, 39, (uint8_t)(200 * a));
    float th = std::floor(g.cellW * 9.0f / 16.0f);
    if (g.cellH > g.cellW * 1.5f) th = std::floor(g.cellW * 16.0f / 9.0f);  // Shorts cards
    for (int row = 0; row < 3; row++) {
        float y = r.y + g.headerH + g.padTop + row * (g.cellH + g.gapY);
        if (y > r.y + r.h) break;
        for (int c = 0; c < g.cols; c++) {
            float x = r.x + g.padX + c * (g.cellW + g.gapX);
            g_gfx.roundRect(x, y, g.cellW, th, 10, c1);
            g_gfx.circle(x + 17, y + th + 27, 17, c2);
            g_gfx.roundRect(x + 46, y + th + 12, g.cellW - 60, 14, 4, c2);
            g_gfx.roundRect(x + 46, y + th + 34, (g.cellW - 60) * 0.6f, 12, 4, c2);
        }
    }
}

static void draw_skeleton_rows(const Rect& r, float rowH, bool compact) {
    float a = 0.55f + 0.25f * std::sin((float)g_gfx.time * 3.0f);
    uint32_t c1 = rgba(39, 39, 39, (uint8_t)(255 * a)), c2 = rgba(39, 39, 39, (uint8_t)(200 * a));
    for (float y = r.y; y + rowH <= r.y + r.h + rowH * 0.5f; y += rowH) {
        float th = rowH - (compact ? 14 : 22), tw = th * 16.0f / 9.0f;
        float x = r.x + (compact ? 10 : 18);
        g_gfx.roundRect(x, y + (rowH - th) / 2, tw, th, 8, c1);
        float tx = x + tw + 14, w = r.x + r.w - tx - 30;
        g_gfx.roundRect(tx, y + (rowH - th) / 2 + 2, w * 0.9f, compact ? 11 : 15, 4, c2);
        g_gfx.roundRect(tx, y + (rowH - th) / 2 + (compact ? 22 : 30), w * 0.5f, compact ? 10 : 12, 4, c2);
    }
}

// ---------------------------------------------------------------- Feed

void Feed::reset() {
    items.clear();
    progress.clear();
    shelf.clear();
    shelfSel = 0;
    shelfScroll_ = 0;
    shelfActivated_ = -1;
    list.reset();
    grid.reset();
    loading = false;
    error.clear();
    continuation.clear();
    retry_ = false;
    gen++;
}

static constexpr int kShelfCards = 5;
static constexpr float kShelfGap = 12, kShelfTitleH = 44, kShelfTextH = 58;

float Feed::shelfCardW(const Rect& r) const {
    float pad = gridLike() ? grid.padX : 18;
    return std::floor((r.w - 2 * pad - (kShelfCards - 1) * kShelfGap) / kShelfCards);
}

void Feed::sync(const Rect& r) {
    int n = (int)items.size();
    list.count = grid.count = n;
    list.rowH = rowH > 0 ? rowH : style == FeedStyle::Rows ? 150.0f : 92.0f;
    if (style == FeedStyle::Grid) grid.layout(r.w, columns, showAvatars ? 96.0f : 90.0f);
    if (style == FeedStyle::Shorts) {
        grid.layout(r.w, 5, 0);
        grid.cellH = std::floor(grid.cellW * 16.0f / 9.0f) + kShelfTextH;
    }
    if (list.sel >= n) list.sel = std::max(0, n - 1);
    if (grid.sel >= n) grid.sel = std::max(0, n - 1);
    bool shelfOn = showShelf && !shelf.empty() && n > 0 && (style == FeedStyle::Grid || style == FeedStyle::Rows);
    float bandH = shelfOn ? kShelfTitleH + std::floor(shelfCardW(r) * 16.0f / 9.0f) + kShelfTextH + 12 : 0;
    grid.bandH = list.bandH = bandH;
    grid.bandRow = shelfOn ? std::min(shelfRow, grid.rows()) : -1;
    list.bandAt = shelfOn ? std::min(shelfAt, n) : -1;
    if (shelfSel >= (int)shelf.size()) shelfSel = std::max(0, (int)shelf.size() - 1);
}

void Feed::drawShelf(const Rect& r, float y) {
    float pad = gridLike() ? grid.padX : 18;
    float cw = shelfCardW(r), th = std::floor(cw * 16.0f / 9.0f);
    bool foc = focused() && shelfFocused();
    g_gfx.rect(r.x + pad, y + 2, r.w - 2 * pad, 1, col::divider);
    draw_icon(Icon::ShortsFilled, r.x + pad + 12, y + 24, 24, col::red, col::text);
    g_gfx.text(r.x + pad + 32, y + 24 - g_gfx.lineHeight(0.98f) / 2, col::text, 0.98f, shelfTitle, W_BOLD);
    // Keep the focused card in view.
    float maxScroll = std::max(0.0f, shelf.size() * (cw + kShelfGap) - kShelfGap - (r.w - 2 * pad));
    float want = shelfScroll_;
    float left = shelfSel * (cw + kShelfGap);
    if (left < want) want = left;
    if (left + cw > want + r.w - 2 * pad) want = left + cw - (r.w - 2 * pad);
    want = std::max(0.0f, std::min(want, maxScroll));
    shelfScroll_ += (want - shelfScroll_) * 0.3f;
    if (std::fabs(want - shelfScroll_) < 0.5f) shelfScroll_ = want;
    float cy = y + kShelfTitleH;
    for (size_t i = 0; i < shelf.size(); i++) {
        float cx = r.x + pad + i * (cw + kShelfGap) - shelfScroll_;
        if (cx > r.x + r.w) break;
        if (cx + cw < r.x) continue;
        draw_shorts_card(shelf[i], Rect{cx, cy, cw, th + kShelfTextH}, foc && (int)i == shelfSel);
    }
    g_gfx.rect(r.x + pad, y + kShelfTitleH + th + kShelfTextH + 8, r.w - 2 * pad, 1, col::divider);
}

bool Feed::append(const Page& p) {
    if (style == FeedStyle::Shorts) {
        // A Shorts grid shows the page's Shorts (and any Shorts among the regular items).
        Page q;
        q.continuation = p.continuation;
        q.items = p.shorts;
        for (const Item& it : p.items)
            if (it.shorts) q.items.push_back(it);
        Page plain = q;
        plain.shorts.clear();
        size_t before = items.size();
        for (const Item& it : plain.items) {
            if (hide && hide(it)) continue;
            bool dup = false;
            for (size_t k = before > 60 ? before - 60 : 0; k < before && !dup; k++) dup = items[k].id == it.id;
            if (!dup) items.push_back(it);
        }
        continuation = items.size() > before ? p.continuation : std::string();
        list.count = grid.count = (int)items.size();
        return items.size() > before;
    }
    if (shelf.empty() && !p.shorts.empty()) {
        for (const Item& it : p.shorts)
            if (!hide || !hide(it)) shelf.push_back(it);
    }
    size_t before = items.size();
    for (const Item& it : p.items) {
        if (hide && hide(it)) continue;
        bool dup = false;
        // Continuations sometimes repeat the last few items of the previous page.
        for (size_t k = before > 40 ? before - 40 : 0; k < before; k++) {
            if (items[k].id == it.id && items[k].type == it.type) {
                dup = true;
                break;
            }
        }
        if (!dup) items.push_back(it);
    }
    continuation = items.size() > before ? p.continuation : std::string();
    list.count = grid.count = (int)items.size();
    return items.size() > before;
}

void Feed::setSel(int i) {
    list.sel = grid.sel = std::max(0, std::min(i, (int)items.size() - 1));
}

int Feed::prune() {
    if (!hide) return 0;
    int s = sel(), removed = 0, removedBefore = 0;
    bool withProgress = progress.size() == items.size();
    size_t o = 0;
    for (size_t k = 0; k < items.size(); k++) {
        if (hide(items[k])) {
            removed++;
            if ((int)k < s) removedBefore++;
            continue;
        }
        if (o != k) {
            items[o] = std::move(items[k]);
            if (withProgress) progress[o] = progress[k];
        }
        o++;
    }
    items.resize(o);
    if (withProgress) progress.resize(o);
    size_t before = shelf.size();
    shelf.erase(std::remove_if(shelf.begin(), shelf.end(), [this](const Item& it) { return hide(it); }), shelf.end());
    removed += (int)(before - shelf.size());
    list.count = grid.count = (int)items.size();
    if (!items.empty()) {
        int n = (int)items.size();
        int ns = std::max(0, std::min(s - removedBefore, n - 1));
        list.sel = grid.sel = ns;
    }
    if (shelfSel >= (int)shelf.size()) shelfSel = std::max(0, (int)shelf.size() - 1);
    return removed;
}

void Feed::scrollToTop() {
    list.reset();
    grid.sel = 0;
    grid.scrollToTop();
}

int Feed::update(Input& in, const Rect& r) {
    sync(r);
    if (items.empty()) {
        if (!loading && !error.empty()) {
            if (focused() && in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                retry_ = true;
            }
            if (retryBtn_.w > 0 && in.tapIn(retryBtn_.x - 6, retryBtn_.y - 6, retryBtn_.w + 12, retryBtn_.h + 12)) {
                in.consumeTap();
                retry_ = true;
            }
        }
        if (gridLike()) grid.update(in, r);  // keeps the header scrollable
        return -1;
    }
    if (shelfFocused() && focused()) {
        int n = (int)shelf.size();
        if (in.repeat(BTN_RIGHT) && shelfSel + 1 < n) {
            in.consume(BTN_RIGHT);
            shelfSel++;
        }
        if (in.repeat(BTN_LEFT) && shelfSel > 0) {
            in.consume(BTN_LEFT);
            shelfSel--;
        }
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            shelfActivated_ = shelfSel;
        }
    }
    // Taps on shelf cards.
    bool grid_ = gridLike();
    if (in.tapped && r.contains(in.tapX, in.tapY) && (grid_ ? grid.hasBand() : list.hasBand())) {
        float top = r.y + (grid_ ? grid.bandTop() - grid.scroll() : list.bandTop() - list.scroll()) + kShelfTitleH;
        float pad = grid_ ? grid.padX : 18, cw = shelfCardW(r);
        float h = std::floor(cw * 16.0f / 9.0f) + kShelfTextH;
        if (in.tapY >= top && in.tapY < top + h) {
            float u = in.tapX - r.x - pad + shelfScroll_;
            int idx = (int)(u / (cw + kShelfGap));
            if (u >= 0 && idx < (int)shelf.size() && u - idx * (cw + kShelfGap) <= cw) {
                in.consumeTap();
                shelfSel = idx;
                (grid_ ? grid.bandSel : list.bandSel) = true;
                shelfActivated_ = idx;
            }
        }
    }
    return grid_ ? grid.update(in, r) : list.update(in, r);
}

bool Feed::wantsMore(const Rect& r) const {
    if (loading || continuation.empty() || !error.empty() || items.empty()) return false;
    if (gridLike()) return grid.nearEnd(2) || grid.scrolledNearEnd(r);
    return list.nearEnd(4) || list.scrolledNearEnd(r);
}

void Feed::draw(const Rect& r, const std::function<void(float y)>& header) {
    sync(r);
    retryBtn_ = Rect{};
    if (items.empty()) {
        float top = r.y;
        if (header && gridLike() && grid.headerH > 0) {
            g_gfx.clip(r);
            header(r.y - grid.scroll());
            g_gfx.unclip();
            top = r.y + grid.headerH - grid.scroll();
        }
        Rect rest{r.x, top, r.w, r.y + r.h - top};
        if (loading) {
            if (gridLike()) draw_skeleton_grid(Rect{r.x, top - grid.headerH, r.w, rest.h + grid.headerH}, grid);
            else draw_skeleton_rows(rest, list.rowH, style == FeedStyle::Compact);
        } else if (!error.empty()) {
            retryBtn_ = draw_empty_state(rest, Icon::Info, "Something went wrong", error, "Retry", focused());
        } else {
            draw_empty_state(rest, emptyIcon, emptyTitle, emptyText);
        }
        return;
    }
    // Explicit progress (History) wins; otherwise show how much of each video was watched, like YouTube.
    auto prog = [&](int i) {
        if (i < (int)progress.size()) return progress[i];
        return items[i].type == ItemType::Video ? g_app->store.watchedFraction(items[i].id) : -1.0f;
    };
    auto band = [&](float y) { drawShelf(r, y); };
    if (style == FeedStyle::Shorts) {
        grid.draw(r, [&](int i, const Rect& c, bool sel) { draw_shorts_card(items[i], c, sel); }, header);
    } else if (style == FeedStyle::Grid) {
        grid.draw(r, [&](int i, const Rect& c, bool sel) { draw_video_card(items[i], c, sel, showAvatars, prog(i)); },
                  header, band);
    } else {
        bool compact = style == FeedStyle::Compact;
        list.draw(r, [&](int i, const Rect& row, bool sel) {
            if (compact) draw_compact_row(items[i], row, sel);
            else draw_result_row(items[i], row, sel, prog(i));
        }, band);
    }
    if (loading) {
        float cy = r.y + r.h - 22;
        g_gfx.circle(r.x + r.w / 2, cy, 18, col::bg);
        g_gfx.spinner(r.x + r.w / 2, cy, 11, col::text2);
    } else if (!error.empty()) {
        float w = std::min(r.w - 32, (float)g_gfx.width(error, 0.66f) + 32);
        float x = r.x + (r.w - w) / 2, y = r.y + r.h - 40;
        g_gfx.roundRect(x, y, w, 30, 6, col::menu);
        g_gfx.textCentered(r.x + r.w / 2, y + (30 - g_gfx.lineHeight(0.66f)) / 2, col::text, 0.66f,
                           g_gfx.ellipsize(error, w - 24, 0.66f));
    }
    // Pull-to-refresh: a refresh button slides down with the finger and lights up once letting
    // go would refresh.
    float pull = pullDistance();
    if (pull > 4) {
        float t = std::min(1.0f, pull / PULL_TO_REFRESH);
        float cx = r.x + r.w / 2, cy = r.y + std::min(pull, PULL_TO_REFRESH * 1.3f) * 0.5f;
        uint8_t a = (uint8_t)(t * 255);
        g_gfx.circle(cx, cy, 19, with_alpha(col::menu, a));
        draw_icon(Icon::Refresh, cx, cy, 22, with_alpha(t >= 1 ? col::text : col::text3, a), col::menu);
    }
}

bool Feed::atVeryTop() const {
    if (gridLike()) return grid.scroll() <= 1 && grid.sel < grid.cols && !grid.bandSel;
    return list.scroll() <= 1 && list.sel == 0 && !list.bandSel;
}

// ---------------------------------------------------------------- Menu

static constexpr float kMenuItemH = 40, kMenuScale = 0.8f;

void Menu::open(const std::string& title, const std::vector<std::string>& items, int sel, int checked) {
    title_ = title;
    items_ = items;
    sel_ = std::max(0, std::min(sel, (int)items.size() - 1));
    checked_ = checked;
    first_ = 0;
    icons_.clear();
    checks_.clear();
    checklist_ = false;
    open_ = !items_.empty();
}

void Menu::openChecklist(const std::string& title, const std::vector<std::string>& items,
                         const std::vector<int>& checks, int sel) {
    open(title, items, sel, -1);
    checks_ = checks;
    checks_.resize(items_.size(), -1);
    checklist_ = true;
}

int Menu::update(Input& in) {
    if (!open_) return -1;
    int n = (int)items_.size();
    if (in.repeat(BTN_DOWN)) sel_ = (sel_ + 1) % n;
    if (in.repeat(BTN_UP)) sel_ = (sel_ + n - 1) % n;
    if (in.tapped) {
        in.consumeTap();
        if (box_.contains(in.tapX, in.tapY)) {
            int idx = first_ + (int)std::floor((in.tapY - itemY0_) / kMenuItemH);
            if (in.tapY >= itemY0_ && idx >= 0 && idx < n) {
                sel_ = idx;
                if (!checklist_) open_ = false;
                return idx;
            }
            return -1;
        }
        open_ = false;
        return -2;
    }
    if (in.pressed(BTN_CONFIRM)) {
        in.consume(BTN_CONFIRM);
        if (!checklist_) open_ = false;
        return sel_;
    }
    if (in.pressed(BTN_CANCEL) || in.pressed(BTN_SQUARE)) {
        in.consume(BTN_CANCEL | BTN_SQUARE);
        open_ = false;
        return -2;
    }
    in.consume(0xFFFFFFFF);  // modal
    return -1;
}

void Menu::draw() {
    if (!open_) return;
    float textW = 0;
    for (auto& s : items_) textW = std::max(textW, (float)g_gfx.width(s, kMenuScale));
    bool checks = checked_ >= 0 || checklist_;
    bool icons = icons_.size() == items_.size();
    float pad = (checks ? 52 : 24) + (icons ? 40 : 0);
    float w = std::max(300.0f, std::min(600.0f, textW + pad + 32));
    const auto& titleLines = g_gfx.wrap(title_, w - 40, 0.74f, 3);
    float titleH = title_.empty() ? 8 : titleLines.size() * g_gfx.lineHeight(0.74f) + 24;
    int visible = std::min((int)items_.size(), (int)((SCREEN_H - 60 - titleH) / kMenuItemH));
    if (sel_ < first_) first_ = sel_;
    if (sel_ >= first_ + visible) first_ = sel_ - visible + 1;
    float h = titleH + visible * kMenuItemH + 8;
    box_ = Rect{std::floor((SCREEN_W - w) / 2), std::floor((SCREEN_H - h) / 2), w, h};
    g_gfx.rect(0, 0, SCREEN_W, SCREEN_H, rgba(0, 0, 0, 150));
    g_gfx.roundRect(box_.x, box_.y, w, h, 12, col::menu);
    float y = box_.y + 14;
    if (!title_.empty()) {
        for (size_t i = 0; i < titleLines.size(); i++)
            g_gfx.textLine(box_.x + 20, y + i * g_gfx.lineHeight(0.74f), w - 40, col::text2, 0.74f, titleLines[i]);
        y = box_.y + titleH - 4;
        g_gfx.rect(box_.x, y - 1, w, 1, col::divider);
        y += 4;
    } else {
        y = box_.y + 8;
    }
    itemY0_ = y;
    for (int k = 0; k < visible; k++) {
        int i = first_ + k;
        float iy = y + k * kMenuItemH;
        bool sel = i == sel_;
        if (sel) g_gfx.rect(box_.x, iy, w, kMenuItemH, col::menuSel);
        float tx = box_.x + 20;
        uint32_t rowBg = sel ? col::menuSel : col::menu;
        if (checklist_) {
            int st = checks_[i];
            float bx = tx, by = iy + (kMenuItemH - 20) / 2;
            if (st == 1) {
                g_gfx.roundRect(bx, by, 20, 20, 3, col::text);
                draw_icon(Icon::Check, bx + 10, by + 10, 16, col::onLight, col::text);
            } else if (st == 0) {
                g_gfx.roundFrame(bx, by, 20, 20, 3, 2, col::text);
            } else if (icons) {
                draw_icon(icons_[i], bx + 10, iy + kMenuItemH / 2, 22, col::text, rowBg);
            }
            tx += 36;
        } else {
            if (checks) {
                if (i == checked_) draw_icon(Icon::Check, tx + 9, iy + kMenuItemH / 2, 18, col::text, rowBg);
                tx += 32;
            }
            if (icons) {
                draw_icon(icons_[i], tx + 11, iy + kMenuItemH / 2, 22, col::text, rowBg);
                tx += 40;
            }
        }
        g_gfx.text(tx, iy + (kMenuItemH - g_gfx.lineHeight(kMenuScale)) / 2, col::text, kMenuScale,
                   g_gfx.ellipsize(items_[i], box_.x + w - tx - 16, kMenuScale));
    }
    // Scroll indicator when not every option fits.
    if ((int)items_.size() > visible) {
        float trackH = visible * kMenuItemH - 8, barH = std::max(20.0f, trackH * visible / items_.size());
        float barY = y + 4 + (trackH - barH) * first_ / std::max(1, (int)items_.size() - visible);
        g_gfx.roundRect(box_.x + w - 7, barY, 3, barH, 1.5f, rgba(255, 255, 255, 60));
    }
}

}  // namespace vt
