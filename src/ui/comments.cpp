#include "comments.h"

#include <algorithm>

#include "../core/util.h"
#include "app.h"

namespace vt {

namespace {
constexpr float kHeaderH = 44, kAvatarCol = 44;
constexpr float kHeadScale = 0.58f, kTextScale = 0.64f;
}  // namespace

void CommentsPanel::start(const std::string& continuation, const std::vector<SortOption>& sorts,
                          const std::string& count) {
    clear();
    started_ = true;
    first_ = continuation;
    sorts_ = sorts;
    count_ = count;
    load(false);
}

void CommentsPanel::clear() {
    gen_++;
    top_ = Thread();
    rep_ = Thread();
    replies_ = header_ = started_ = false;
    sort_ = 0;
}

void CommentsPanel::closeReplies() {
    replies_ = false;
    gen_++;
    rep_ = Thread();
    top_.loading = false;
}

void CommentsPanel::load(bool more) {
    Thread& t = cur();
    std::string cont = more ? t.cont : replies_ ? parent_.repliesToken : first_;
    if (!more && !replies_ && sort_ > 0 && sort_ < (int)sorts_.size()) cont = sorts_[sort_].token;
    if (cont.empty()) {
        t.loading = false;
        if (!more) t.error = "Comments are turned off or unavailable for this video.";
        return;
    }
    t.loading = true;
    auto page = std::make_shared<CommentsPage>();
    std::weak_ptr<bool> alive = alive_;
    uint32_t gen = gen_;
    bool rep = replies_;
    g_app->jobs.post([page, cont] { *page = g_app->yt.comments(cont); }, [this, alive, page, gen, rep] {
        if (!alive.lock() || gen != gen_ || rep != replies_) return;
        Thread& th = cur();
        th.loading = false;
        if (!page->error.empty() && page->comments.empty()) {
            th.error = page->error;
            return;
        }
        th.cont = page->continuation;
        for (auto& c : page->comments) th.items.push_back(std::move(c));
        th.list.count = (int)th.items.size();
        th.list.heights.clear();  // recomputed for the panel width
        if (th.items.empty() || (rep && th.items.size() == 1)) th.error = rep ? "No replies yet." : "No comments yet.";
    });
}

void CommentsPanel::openReplies(const Comment& c) {
    parent_ = c;
    replies_ = true;
    header_ = false;
    gen_++;
    rep_ = Thread();
    rep_.items.push_back(c);  // the thread's own comment heads the list
    Comment& p = rep_.items.back();
    p.replies.clear();
    rep_.list.count = 1;
    load(false);
}

void CommentsPanel::sortMenu() {
    if (sorts_.size() < 2) {
        g_app->toast("This video has one comment order");
        return;
    }
    std::vector<std::string> labels;
    for (auto& s : sorts_) labels.push_back(s.title);
    std::weak_ptr<bool> alive = alive_;
    g_app->showMenu("Sort comments", labels, [this, alive](int i) {
        if (!alive.lock() || i == sort_) return;
        sort_ = i;
        gen_++;
        top_ = Thread();
        load(false);
    }, sort_, sort_, {Icon::ThumbUp, Icon::History});
}

float CommentsPanel::rowHeight(const Comment& c, float w) const {
    const auto& lines = g_gfx.wrap(c.text, w - kAvatarCol - 16, kTextScale, 12);
    float h = 10 + g_gfx.lineHeight(kHeadScale) + 3 + lines.size() * g_gfx.lineHeight(kTextScale) + 6;
    h += 22;  // like / dislike / reply row
    if (!c.replies.empty() && !c.repliesToken.empty()) h += 26;
    return h + 6;
}

void CommentsPanel::update(Input& in, const Rect& r, bool focused) {
    width_ = r.w;
    Thread& t = cur();
    Rect lr{r.x, r.y + kHeaderH, r.w, r.h - kHeaderH};
    if ((int)t.list.heights.size() != (int)t.items.size()) {
        t.list.heights.clear();
        for (size_t k = 0; k < t.items.size(); k++)
            t.list.heights.push_back(rowHeight(t.items[k], r.w - (replies_ && k > 0 ? 28 : 0)));
    }
    t.list.count = (int)t.items.size();
    // Header taps: sort button / back from replies.
    if (in.tapped && in.tapIn(r.x, r.y, r.w, kHeaderH)) {
        in.consumeTap();
        if (replies_) closeReplies();
        else sortMenu();
        return;
    }
    if (!focused) {
        t.list.focused = false;
        t.list.update(in, lr);
        return;
    }
    if (replies_ && in.pressed(BTN_CANCEL)) {
        in.consume(BTN_CANCEL);
        closeReplies();
        return;
    }
    if (header_) {
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            if (replies_) closeReplies();
            else sortMenu();
        }
        if (in.pressed(BTN_DOWN) && !t.items.empty()) {
            in.consume(BTN_DOWN);
            header_ = false;
        }
        t.list.focused = false;
        t.list.update(in, lr);
        return;
    }
    t.list.focused = true;
    bool atTop = t.items.empty() || t.list.sel == 0;
    int a = t.list.update(in, lr);
    if (in.pressed(BTN_UP) && atTop) {
        in.consume(BTN_UP);
        header_ = true;
    }
    if (a >= 0 && a < (int)t.items.size()) {
        const Comment& c = t.items[a];
        if (!replies_ && !c.repliesToken.empty()) openReplies(c);
    }
    Thread& th = cur();
    if (!th.loading && !th.cont.empty() && (th.list.nearEnd(3) || th.list.scrolledNearEnd(lr))) load(true);
}

void CommentsPanel::drawRow(const Comment& c, const Rect& r, bool sel, bool parent) {
    uint32_t bg = sel ? col::rowHi : col::bg;
    if (sel) g_gfx.roundRect(r.x + 4, r.y + 2, r.w - 12, r.h - 4, 10, bg);
    float x = r.x + kAvatarCol, w = r.w - kAvatarCol - 16, y = r.y + 10;
    float ar = parent || !c.reply ? 14 : 11;
    draw_avatar(c.avatar, c.author, r.x + 24, y + ar, ar, bg);
    float hx = x;
    if (c.pinned) {
        g_gfx.text(hx, y, col::text2, kHeadScale, "Pinned");
        hx += g_gfx.width("Pinned", kHeadScale) + 8;
    }
    if (c.creator) {
        float aw = (float)g_gfx.width(c.author, kHeadScale, W_MEDIUM) + 12;
        g_gfx.roundRect(hx - 2, y - 1, aw, g_gfx.lineHeight(kHeadScale) + 2, 8, rgba(113, 113, 113));
        g_gfx.text(hx + 4, y, col::text, kHeadScale, c.author, W_MEDIUM);
        hx += aw + 6;
    } else {
        std::string a = g_gfx.ellipsize(c.author, w * 0.6f, kHeadScale, W_MEDIUM);
        g_gfx.text(hx, y, col::text, kHeadScale, a, W_MEDIUM);
        hx += g_gfx.width(a, kHeadScale, W_MEDIUM) + 6;
    }
    if (!c.published.empty())
        g_gfx.text(hx, y, col::text2, kHeadScale, g_gfx.ellipsize(c.published, x + w - hx, kHeadScale));
    y += g_gfx.lineHeight(kHeadScale) + 3;
    y += g_gfx.paragraph(x, y, w, col::text, kTextScale, c.text, 12) + 6;
    // Like count, dislike and reply, as on the site (read-only here).
    draw_icon(Icon::ThumbUp, x + 8, y + 9, 15, col::text2, bg);
    float fx = x + 22;
    if (!c.likes.empty()) {
        g_gfx.text(fx, y + 1, col::text2, kHeadScale, c.likes);
        fx += g_gfx.width(c.likes, kHeadScale) + 12;
    } else {
        fx += 6;
    }
    draw_icon(Icon::ThumbDown, fx + 8, y + 9, 15, col::text2, bg);
    y += 22;
    if (!parent && !c.replies.empty() && !c.repliesToken.empty()) {
        draw_icon(Icon::ChevronDown, x + 8, y + 11, 14, col::blue, bg);
        g_gfx.text(x + 22, y + 3, col::blue, kHeadScale, c.replies, W_MEDIUM);
    }
}

void CommentsPanel::draw(const Rect& r, bool focused) {
    width_ = r.w;
    Thread& t = cur();
    // Header: "Comments 1.2K  [Sort by]" or "< Replies".
    float hy = r.y + (kHeaderH - g_gfx.lineHeight(0.84f)) / 2;
    bool hf = focused && header_;
    if (replies_) {
        if (hf) g_gfx.circle(r.x + 24, r.y + kHeaderH / 2, 18, col::raisedHi);
        draw_icon(Icon::Back, r.x + 24, r.y + kHeaderH / 2, 20, col::text, hf ? col::raisedHi : col::bg);
        if (hf) g_gfx.ring(r.x + 24, r.y + kHeaderH / 2, 20, 2.5f, col::focus);
        g_gfx.text(r.x + 50, hy, col::text, 0.84f, "Replies", W_BOLD);
    } else {
        g_gfx.text(r.x + 14, hy, col::text, 0.84f, "Comments", W_BOLD);
        float cx = r.x + 14 + g_gfx.width("Comments", 0.84f, W_BOLD) + 8;
        if (!count_.empty()) g_gfx.text(cx, hy + 2, col::text2, 0.72f, count_);
        // Sort button on the right.
        const char* label = "Sort by";
        float bw = g_gfx.width(label, 0.66f, W_MEDIUM) + 44;
        float bx = r.x + r.w - bw - 10, by = r.y + 6;
        g_gfx.roundRect(bx, by, bw, 32, 16, hf ? col::raisedHi : col::bg);
        draw_icon(Icon::Sort, bx + 18, by + 16, 18, col::text, hf ? col::raisedHi : col::bg);
        g_gfx.text(bx + 34, by + (32 - g_gfx.lineHeight(0.66f)) / 2, col::text, 0.66f, label, W_MEDIUM);
        if (hf) g_gfx.roundFrame(bx - 3, by - 3, bw + 6, 38, 19, 2.5f, col::focus);
    }
    Rect lr{r.x, r.y + kHeaderH, r.w, r.h - kHeaderH};
    if ((int)t.list.heights.size() != (int)t.items.size()) {
        t.list.heights.clear();
        for (size_t k = 0; k < t.items.size(); k++)
            t.list.heights.push_back(rowHeight(t.items[k], r.w - (replies_ && k > 0 ? 28 : 0)));
    }
    t.list.count = (int)t.items.size();
    if (t.items.empty() || (replies_ && t.items.size() == 1 && !t.loading && !t.error.empty())) {
        if (t.loading) draw_loading(lr);
        else if (t.items.empty()) draw_empty_state(lr, Icon::Comment, replies_ ? "Replies" : "Comments", t.error);
        if (t.items.empty()) return;
    }
    t.list.focused = focused && !header_;
    t.list.draw(lr, [&](int i, const Rect& row, bool sel) {
        bool parent = replies_ && i == 0;
        Rect rr = row;
        if (replies_ && i > 0) {  // replies are indented under the thread
            rr.x += 28;
            rr.w -= 28;
        }
        drawRow(t.items[i], rr, sel, parent);
    });
    if (t.loading) g_gfx.spinner(r.x + r.w / 2, r.y + r.h - 18, 9, col::text2);
}

}  // namespace vt
