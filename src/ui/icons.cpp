#include "icons.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace vt {

namespace {

// Icons are designed on a 24x24 grid; P maps grid units to screen pixels.
struct P {
    float cx, cy, s;
    float x(float u) const { return cx + (u - 12.0f) * s / 24.0f; }
    float y(float v) const { return cy + (v - 12.0f) * s / 24.0f; }
    float d(float units) const { return units * s / 24.0f; }
};

// Polyline with round joins, `t` grid units thick.
void poly(const P& p, std::initializer_list<float> pts, float t, uint32_t c) {
    const float* a = pts.begin();
    size_t n = pts.size() / 2;
    float th = p.d(t);
    for (size_t i = 0; i + 1 < n; i++)
        g_gfx.line(p.x(a[i * 2]), p.y(a[i * 2 + 1]), p.x(a[i * 2 + 2]), p.y(a[i * 2 + 3]), th, c);
    if (th >= 2.0f)
        for (size_t i = 0; i < n; i++) g_gfx.circle(p.x(a[i * 2]), p.y(a[i * 2 + 1]), th * 0.5f, c);
}

void rrect(const P& p, float u0, float v0, float u1, float v1, float r, uint32_t c) {
    g_gfx.roundRect(p.x(u0), p.y(v0), p.d(u1 - u0), p.d(v1 - v0), p.d(r), c);
}

void rframe(const P& p, float u0, float v0, float u1, float v1, float r, float t, uint32_t c) {
    g_gfx.roundFrame(p.x(u0), p.y(v0), p.d(u1 - u0), p.d(v1 - v0), p.d(r), std::max(1.0f, p.d(t)), c);
}

void tri(const P& p, float u0, float v0, float u1, float v1, float u2, float v2, uint32_t c) {
    g_gfx.triangle(p.x(u0), p.y(v0), p.x(u1), p.y(v1), p.x(u2), p.y(v2), c);
}

void dot(const P& p, float u, float v, float r, uint32_t c) { g_gfx.circle(p.x(u), p.y(v), p.d(r), c); }

void clock(const P& p, uint32_t c) {
    g_gfx.ring(p.x(12), p.y(12), p.d(9), std::max(1.5f, p.d(2)), c);
    poly(p, {12, 7, 12, 12, 15.5f, 14}, 2, c);
}

}  // namespace

void draw_icon(Icon icon, float cx, float cy, float size, uint32_t c, uint32_t bg) {
    P p{cx, cy, size};
    const float T = 2.0f;  // stroke width in grid units
    switch (icon) {
        case Icon::Home:
            poly(p, {3.5f, 11, 12, 3.5f, 20.5f, 11}, T, c);
            poly(p, {6, 9.5f, 6, 20, 18, 20, 18, 9.5f}, T, c);
            poly(p, {10, 20, 10, 14.5f, 14, 14.5f, 14, 20}, T, c);
            break;
        case Icon::HomeFilled:
            tri(p, 2.5f, 11.5f, 12, 2.5f, 21.5f, 11.5f, c);
            rrect(p, 5, 10, 19, 21, 0.5f, c);
            rrect(p, 10, 14, 14, 21, 0, bg);
            break;
        case Icon::Subscriptions:
            poly(p, {7, 3, 17, 3}, T, c);
            poly(p, {5, 6.5f, 19, 6.5f}, T, c);
            rframe(p, 2.5f, 9.5f, 21.5f, 21.5f, 2.5f, T, c);
            tri(p, 10, 12.5f, 10, 18.5f, 15.5f, 15.5f, c);
            break;
        case Icon::SubscriptionsFilled:
            poly(p, {7, 3, 17, 3}, T, c);
            poly(p, {5, 6.5f, 19, 6.5f}, T, c);
            rrect(p, 2.5f, 9.5f, 21.5f, 21.5f, 2.5f, c);
            tri(p, 10, 12.5f, 10, 18.5f, 15.5f, 15.5f, bg);
            break;
        case Icon::You:
            g_gfx.ring(p.x(12), p.y(7.5f), p.d(4.2f), std::max(1.5f, p.d(T)), c);
            rframe(p, 4, 13.5f, 20, 22, 4, T, c);
            break;
        case Icon::YouFilled:
            dot(p, 12, 7.5f, 4.5f, c);
            rrect(p, 3.5f, 13.5f, 20.5f, 22, 4.5f, c);
            break;
        case Icon::Search:
            g_gfx.ring(p.x(10), p.y(10), p.d(6.5f), std::max(1.5f, p.d(T)), c);
            poly(p, {15, 15, 20.5f, 20.5f}, 2.4f, c);
            break;
        case Icon::Settings: {
            float r1 = p.d(8.3f);
            for (int i = 0; i < 8; i++) {  // teeth
                float a = i * 0.785398f;
                float ca = std::cos(a), sa = std::sin(a);
                g_gfx.line(cx + ca * r1 * 0.6f, cy + sa * r1 * 0.6f, cx + ca * r1 * 1.22f, cy + sa * r1 * 1.22f, p.d(3.2f),
                           c);
            }
            g_gfx.circle(cx, cy, r1, c);
            g_gfx.circle(cx, cy, p.d(3.2f), bg);
            break;
        }
        case Icon::History:
        case Icon::WatchLater:
            clock(p, c);
            break;
        case Icon::Back:
            poly(p, {20, 12, 5, 12}, T, c);
            poly(p, {11, 5.5f, 4.5f, 12, 11, 18.5f}, T, c);
            break;
        case Icon::More:
            dot(p, 12, 5, 1.9f, c);
            dot(p, 12, 12, 1.9f, c);
            dot(p, 12, 19, 1.9f, c);
            break;
        case Icon::Play:
            tri(p, 7, 4, 7, 20, 20, 12, c);
            break;
        case Icon::Pause:
            rrect(p, 6, 4.5f, 10, 19.5f, 0.5f, c);
            rrect(p, 14, 4.5f, 18, 19.5f, 0.5f, c);
            break;
        case Icon::Close:
            poly(p, {6, 6, 18, 18}, T, c);
            poly(p, {18, 6, 6, 18}, T, c);
            break;
        case Icon::Check:
            poly(p, {4.5f, 12.5f, 9.5f, 17.5f, 19.5f, 6.5f}, T, c);
            break;
        case Icon::Save:
            poly(p, {3, 6, 15, 6}, T, c);
            poly(p, {3, 11, 15, 11}, T, c);
            poly(p, {3, 16, 10, 16}, T, c);
            poly(p, {17.5f, 12, 17.5f, 21}, T, c);
            poly(p, {13, 16.5f, 22, 16.5f}, T, c);
            break;
        case Icon::Captions:
            rframe(p, 2, 5, 22, 19, 2.5f, T, c);
            g_gfx.textCentered(cx, cy - g_gfx.lineHeight(size / 48.0f) * 0.55f, c, size / 48.0f, "CC", true);
            break;
        case Icon::Fullscreen:
            poly(p, {3, 9, 3, 3, 9, 3}, T, c);
            poly(p, {15, 3, 21, 3, 21, 9}, T, c);
            poly(p, {21, 15, 21, 21, 15, 21}, T, c);
            poly(p, {9, 21, 3, 21, 3, 15}, T, c);
            break;
        case Icon::Playlist:
            poly(p, {3, 7, 14, 7}, T, c);
            poly(p, {3, 12, 14, 12}, T, c);
            poly(p, {3, 17, 10, 17}, T, c);
            tri(p, 16, 11, 16, 21, 22, 16, c);
            break;
        case Icon::Next:
            tri(p, 5, 5, 5, 19, 16, 12, c);
            rrect(p, 16.5f, 5, 19, 19, 0.3f, c);
            break;
        case Icon::ArrowUpLeft:
            poly(p, {7, 7, 17.5f, 17.5f}, T, c);
            poly(p, {7, 15, 7, 7, 15, 7}, T, c);
            break;
        case Icon::Comment:
            rframe(p, 3, 4, 21, 17, 3, T, c);
            tri(p, 7, 16, 7, 21.5f, 12.5f, 16, c);
            break;
        case Icon::Info:
            g_gfx.ring(p.x(12), p.y(12), p.d(9), std::max(1.5f, p.d(T)), c);
            dot(p, 12, 7.8f, 1.4f, c);
            poly(p, {12, 11, 12, 17}, T, c);
            break;
        case Icon::Shuffle:
            poly(p, {3, 7, 8, 7, 16, 17, 21, 17}, T, c);
            poly(p, {3, 17, 8, 17, 16, 7, 21, 7}, T, c);
            poly(p, {18, 4, 21, 7, 18, 10}, T, c);
            poly(p, {18, 14, 21, 17, 18, 20}, T, c);
            break;
        case Icon::Trash:
            poly(p, {4, 6.5f, 20, 6.5f}, T, c);
            poly(p, {9, 6.5f, 9, 3.5f, 15, 3.5f, 15, 6.5f}, T, c);
            poly(p, {6, 6.5f, 7, 21, 17, 21, 18, 6.5f}, T, c);
            break;
        case Icon::ChevronRight:
            poly(p, {9, 5, 16, 12, 9, 19}, T, c);
            break;
        case Icon::ChevronDown:
            poly(p, {5, 9, 12, 16, 19, 9}, T, c);
            break;
        case Icon::Menu:
            poly(p, {3.5f, 6, 20.5f, 6}, T, c);
            poly(p, {3.5f, 12, 20.5f, 12}, T, c);
            poly(p, {3.5f, 18, 20.5f, 18}, T, c);
            break;
        case Icon::Shorts:  // a vertical video: tall frame with a play mark
            rframe(p, 6, 2.5f, 18, 21.5f, 3, T, c);
            tri(p, 10, 8.5f, 10, 15.5f, 15.5f, 12, c);
            break;
        case Icon::ShortsFilled:
            rrect(p, 5.5f, 2, 18.5f, 22, 3.5f, c);
            tri(p, 10, 8.5f, 10, 15.5f, 15.5f, 12, bg);
            break;
        case Icon::ThumbUp:
        case Icon::ThumbUpFilled:
        case Icon::ThumbDown:
        case Icon::ThumbDownFilled: {
            // Drawn upright; the "down" variants mirror it top to bottom.
            bool down = icon == Icon::ThumbDown || icon == Icon::ThumbDownFilled;
            bool filled = icon == Icon::ThumbUpFilled || icon == Icon::ThumbDownFilled;
            P q = p;
            auto Y = [&](float v) { return down ? 24 - v : v; };
            float y0 = std::min(Y(10), Y(21)), y1 = std::max(Y(10), Y(21));
            rrect(q, 2.5f, y0, 6.5f, y1, 1, c);  // cuff
            if (filled) {
                rrect(q, 8, y0, 20.5f, y1, 2.5f, c);
                tri(q, 8.5f, Y(10.5f), 11.5f, Y(3), 14.5f, Y(10.5f), c);
                dot(q, 13, Y(4.2f), 1.8f, c);
            } else {
                rframe(q, 8, y0, 20.5f, y1, 2.5f, T, c);
                poly(q, {9, Y(10.5f), 11.5f, Y(4), 13.5f, Y(4.5f), 13.5f, Y(10)}, T, c);
            }
            break;
        }
        case Icon::Share:
            poly(p, {3.5f, 19, 3.5f, 16, 7.5f, 11.5f, 19, 11.5f}, T, c);
            poly(p, {14, 6.5f, 19.5f, 11.5f, 14, 16.5f}, T, c);
            break;
        case Icon::Account:
            g_gfx.ring(p.x(12), p.y(12), p.d(10), std::max(1.5f, p.d(T)), c);
            dot(p, 12, 9.5f, 3.4f, c);
            poly(p, {6.5f, 18.5f, 9, 15.5f, 15, 15.5f, 17.5f, 18.5f}, 2.2f, c);
            break;
        case Icon::Filter:
            poly(p, {3, 7, 21, 7}, T, c);
            poly(p, {3, 17, 21, 17}, T, c);
            dot(p, 15, 7, 2.8f, c);
            dot(p, 15, 7, 1.2f, bg);
            dot(p, 8, 17, 2.8f, c);
            dot(p, 8, 17, 1.2f, bg);
            break;
        case Icon::Sort:
            poly(p, {3, 6, 21, 6}, T, c);
            poly(p, {3, 12, 16, 12}, T, c);
            poly(p, {3, 18, 10, 18}, T, c);
            break;
        case Icon::Loop:
            poly(p, {4.5f, 13, 4.5f, 8, 18.5f, 8}, T, c);
            poly(p, {15.5f, 5, 18.5f, 8, 15.5f, 11}, T, c);
            poly(p, {19.5f, 11, 19.5f, 16, 5.5f, 16}, T, c);
            poly(p, {8.5f, 13, 5.5f, 16, 8.5f, 19}, T, c);
            break;
        case Icon::Plus:
            poly(p, {12, 4.5f, 12, 19.5f}, T, c);
            poly(p, {4.5f, 12, 19.5f, 12}, T, c);
            break;
        case Icon::Lock:
            rframe(p, 8, 3.5f, 16, 14, 4, T, c);
            rrect(p, 5, 10.5f, 19, 21, 2, c);
            break;
        case Icon::SignOut:
            poly(p, {13, 4, 5, 4, 5, 20, 13, 20}, T, c);
            poly(p, {10, 12, 20.5f, 12}, T, c);
            poly(p, {16.5f, 8, 20.5f, 12, 16.5f, 16}, T, c);
            break;
        case Icon::Download:
            poly(p, {12, 3.5f, 12, 15}, T, c);
            poly(p, {7, 10, 12, 15, 17, 10}, T, c);
            poly(p, {4.5f, 20, 19.5f, 20}, T, c);
            break;
        case Icon::Verified:
            dot(p, 12, 12, 10, c);
            poly(p, {7.5f, 12.5f, 10.5f, 15.5f, 16.5f, 9}, 2.4f, bg);
            break;
        case Icon::Liked:
            draw_icon(Icon::ThumbUp, cx, cy, size, c, bg);
            break;
    }
}

void draw_brand_mark(float x, float y, float size, uint32_t bg) {
    float r = size * 0.26f;
    g_gfx.vgradient(x, y, size, size, col::brandTop, col::brandBottom);
    g_gfx.cornerMask(x, y, size, size, r, bg);
    float cx = x + size / 2, cy = y + size / 2;
    g_gfx.ring(cx, cy, size * 0.36f, std::max(1.5f, size * 0.08f), col::text);
    float t = std::max(1.5f, size * 0.11f);
    g_gfx.line(cx - size * 0.17f, cy - size * 0.15f, cx, cy + size * 0.19f, t, col::brandV);
    g_gfx.line(cx + size * 0.17f, cy - size * 0.15f, cx, cy + size * 0.19f, t, col::brandV);
    g_gfx.circle(cx, cy + size * 0.18f, t * 0.5f, col::brandV);
}

}  // namespace vt
