// Reusable UI pieces: list and grid views driven by d-pad and touch, video cards and rows,
// chips, pill buttons, empty states and the option menu.
//
// Focus convention: a view consumes a d-pad direction only when it moves within itself. An
// unconsumed Up or Left "bubbles" to whoever is around it (chips, header, the app's top bar or
// guide rail), so screens compose without knowing about each other.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../core/models.h"
#include "../vita/input.h"
#include "gfx.h"
#include "icons.h"

namespace vt {

// Glyphs from the system font for button hints.
#define G_CROSS "\xC3\x97"         // ×
#define G_CIRCLE "\xE2\x97\x8B"    // ○
#define G_TRIANGLE "\xE2\x96\xB3"  // △
#define G_SQUARE "\xE2\x96\xA1"    // □
#define G_DOT " \xC2\xB7 "         // " · " separator in metadata lines

namespace col {
constexpr uint32_t rowHi = rgba(39, 39, 39);   // focused row background (opaque, so corners can be masked)
constexpr uint32_t rowCur = rgba(29, 29, 29);  // the playing item in a queue
}

class ListView {
public:
    int count = 0;
    int sel = 0;
    float rowH = 104;
    std::vector<float> heights;  // optional per-row heights; used when heights.size() == count
    bool focused = true;
    // Optional band (a horizontal shelf) shown before row `bandAt`. While `bandSel` is set the
    // band has the focus: the owner handles Left / Right / Confirm for it.
    int bandAt = -1;
    float bandH = 0;
    bool bandSel = false;
    bool hasBand() const { return bandAt >= 0 && bandH > 0 && bandAt <= count && count > 0; }
    float bandTop() const { return offsetOf(bandAt) - bandH; }

    // Handles d-pad / touch. Returns the index activated by Confirm or a tap, else -1.
    int update(Input& in, const Rect& r);
    void draw(const Rect& r, const std::function<void(int index, const Rect& row, bool selected)>& drawRow,
              const std::function<void(float y)>& band = nullptr);
    bool nearEnd(int margin = 3) const { return count > 0 && sel >= count - margin; }
    bool scrolledNearEnd(const Rect& r) const { return count > 0 && scroll_ + r.h >= total() - r.h * 0.5f; }
    void reset() {
        sel = 0;
        scroll_ = 0;
        vel_ = 0;
        target_ = -1;
        bandSel = false;
    }
    void scrollToSelection(const Rect& r) { ensureVisible(r, true); }
    // Eases the view by dy pixels without moving the selection.
    void scrollBy(const Rect& r, float dy) {
        float maxScroll = total() > r.h ? total() - r.h : 0;
        float t = (target_ >= 0 ? target_ : scroll_) + dy;
        target_ = t < 0 ? 0 : t > maxScroll ? maxScroll : t;
        vel_ = 0;
    }
    float scroll() const { return scroll_; }
    // Pull-to-refresh: how far a finger has pulled the list down past its top, and whether a
    // pull far enough to refresh was let go of this frame.
    float pullDistance() const { return touchActive_ && scroll_ < 0 ? -scroll_ : 0; }
    bool takePulled() {
        bool p = pulled_;
        pulled_ = false;
        return p;
    }

private:
    float offsetOf(int i) const;
    float heightOf(int i) const;
    float total() const;
    void ensureVisible(const Rect& r, bool snap);
    int indexAt(float y) const;
    float scroll_ = 0, vel_ = 0, target_ = -1;
    bool touchActive_ = false, pulled_ = false;
};

// A grid of equal cells with an optional header that scrolls with the content.
class GridView {
public:
    int count = 0, sel = 0, cols = 3;
    float cellW = 274, cellH = 240, gapX = 16, gapY = 14, padX = 16, padTop = 12;
    float headerH = 0;
    bool focused = true;
    // Optional band (a horizontal shelf) shown before grid row `bandRow`; see ListView.
    int bandRow = -1;
    float bandH = 0;
    bool bandSel = false;
    int rows() const { return (count + cols - 1) / cols; }
    bool hasBand() const { return bandRow >= 0 && bandH > 0 && bandRow <= rows() && count > 0; }
    float bandTop() const { return headerH + padTop + bandRow * (cellH + gapY); }

    // Sizes cells to fill `width` with `columns` video cards.
    void layout(float width, int columns, float textH = 92);
    // Returns the index activated by Confirm or a tap, else -1.
    int update(Input& in, const Rect& r);
    void draw(const Rect& r, const std::function<void(int, const Rect&, bool)>& cell,
              const std::function<void(float y)>& header = nullptr, const std::function<void(float y)>& band = nullptr);
    void reset() {
        sel = 0;
        scroll_ = 0;
        vel_ = 0;
        target_ = -1;
        bandSel = false;
    }
    void scrollToTop() { target_ = 0; }
    float scroll() const { return scroll_; }
    float pullDistance() const { return touchActive_ && scroll_ < 0 ? -scroll_ : 0; }  // see ListView
    bool takePulled() {
        bool p = pulled_;
        pulled_ = false;
        return p;
    }
    bool nearEnd(int rowsLeft = 2) const { return count > 0 && sel / cols >= (count - 1) / cols - rowsLeft + 1; }
    bool scrolledNearEnd(const Rect& r) const { return count > 0 && scroll_ + r.h >= total() - r.h * 0.5f; }
    // Header taps: true if the tap landed in the header (coordinates relative to it are passed back).
    bool headerTapped(const Input& in, const Rect& r, float& hx, float& hy) const;

private:
    float rowTop(int row) const {
        return headerH + padTop + row * (cellH + gapY) + (hasBand() && row >= bandRow ? bandH : 0);
    }
    float total() const;
    void ensureVisible(const Rect& r, bool snap);
    int indexAt(const Rect& r, float x, float y) const;
    float scroll_ = 0, vel_ = 0, target_ = -1;
    bool touchActive_ = false, pulled_ = false;
};

// Pull distance that triggers a refresh.
constexpr float PULL_TO_REFRESH = 80;

// ---------------------------------------------------------------- items

std::string meta_line(const Item& it);  // "1.2M views · 3 days ago"
// Rounded thumbnail with duration / playlist badges and an optional red watch-progress bar.
void draw_thumb(const Item& it, float x, float y, float w, float h, float radius, uint32_t bg = col::bg,
                float progress = -1, bool badges = true);
// Channel avatar; falls back to a colored circle with the channel's initial.
void draw_avatar(const std::string& url, const std::string& name, float cx, float cy, float r, uint32_t bg = col::bg);
void draw_focus_ring(float x, float y, float w, float h, float radius);
void draw_progress_bar(float x, float y, float w, float frac);
// Grid card: thumbnail on top, avatar + title + channel + stats below.
void draw_video_card(const Item& it, const Rect& cell, bool focused, bool showAvatar = true, float progress = -1);
// List row in the style of search results: big thumbnail left, text right.
void draw_result_row(const Item& it, const Rect& r, bool focused, float progress = -1);
// Narrow row for side panels and playlists; index >= 0 shows a position number.
void draw_compact_row(const Item& it, const Rect& r, bool focused, int index = -1, bool current = false);
// Vertical card for Shorts shelves: 9:16 thumbnail, title and views below.
void draw_shorts_card(const Item& it, const Rect& cell, bool focused, uint32_t bg = col::bg);
// Channel name followed by the verified check when `verified`. Returns the width drawn.
float draw_channel_name(float x, float y, uint32_t color, float scale, const std::string& name, bool verified,
                        float maxW, uint32_t bg = col::bg);
// Small grey boxes ("New", "4K", "CC") under search results. Returns the width drawn.
float draw_meta_badges(float x, float y, const std::vector<std::string>& badges, float maxW);

// ---------------------------------------------------------------- controls

constexpr float CHIP_H = 32;
float chip_width(const std::string& s, float scale = 0.72f);
// focusIdx < 0 when the chip bar does not have focus.
void draw_chips(const std::vector<std::string>& names, int selected, int focusIdx, float x, float y,
                float scale = 0.72f, float gap = 12);
// Index of the chip tapped this frame, or -1 (the tap is not consumed).
int chips_hit(const Input& in, const std::vector<std::string>& names, float x, float y, float scale = 0.72f,
              float gap = 12);

enum class PillStyle { Primary, Secondary };
float pill_width(const std::string& label, bool withIcon = false, float h = 36);
// Returns the width drawn.
float draw_pill(float x, float y, const std::string& label, PillStyle style, bool focused, bool withIcon = false,
                Icon icon = Icon::Play, float h = 36);
void draw_switch(float x, float y, bool on);

// Returns the button's rectangle (zero-sized when there is no button).
Rect draw_empty_state(const Rect& r, Icon icon, const std::string& title, const std::string& body,
                      const std::string& button = "", bool buttonFocused = true);
void draw_loading(const Rect& r);

// ---------------------------------------------------------------- Feed

// Grid: video cards; Rows: search-style rows; Compact: narrow rows; Shorts: a grid of 9:16 cards.
enum class FeedStyle { Grid, Rows, Compact, Shorts };

// A list of items with loading / error / empty states, continuation paging and a choice of
// layouts (grid of cards, search-style rows, compact rows).
struct Feed {
    std::vector<Item> items;
    std::vector<float> progress;  // optional watch progress per item (0..1, <0 = none)
    FeedStyle style = FeedStyle::Grid;
    int columns = 3;     // grid style
    float rowH = 0;      // list styles; 0 = the style's default
    bool showAvatars = true;
    ListView list;
    GridView grid;
    bool loading = false;
    std::string error, continuation;
    std::string emptyTitle = "Nothing here yet", emptyText;
    Icon emptyIcon = Icon::Playlist;
    uint32_t gen = 0;  // bumped on reset so stale responses can be dropped
    // Shorts shelf shown between rows (after `shelfRow` grid rows, or `shelfAt` list rows).
    std::vector<Item> shelf;
    std::string shelfTitle = "Shorts";
    int shelfRow = 2, shelfAt = 3, shelfSel = 0;
    bool showShelf = true;
    // Items for which this returns true are left out of appended pages and removed by prune()
    // (recommendations the user marked "Not interested").
    std::function<bool(const Item&)> hide;

    void reset();
    // Appends a page; returns true if it brought new items.
    bool append(const Page& p);
    // Removes the items (and shelf cards) `hide` now matches, keeping the highlight in place.
    // Returns how many were removed.
    int prune();
    bool gridLike() const { return style == FeedStyle::Grid || style == FeedStyle::Shorts; }
    int sel() const { return gridLike() ? grid.sel : list.sel; }
    void setSel(int i);
    void setFocused(bool f) {
        list.focused = f;
        grid.focused = f;
    }
    bool focused() const { return gridLike() ? grid.focused : list.focused; }
    // Returns the activated index or -1.
    int update(Input& in, const Rect& r);
    // True once after the user picked "Retry" on the error state.
    bool takeRetry() {
        bool r = retry_;
        retry_ = false;
        return r;
    }
    // True once after the feed was pulled down past its top and let go.
    bool takeRefresh() {
        bool a = grid.takePulled(), b = list.takePulled();
        return a || b;
    }
    float pullDistance() const { return gridLike() ? grid.pullDistance() : list.pullDistance(); }
    // Whether the feed is scrolled to its very top with the first item highlighted.
    bool atVeryTop() const;
    bool wantsMore(const Rect& r) const;
    // The item under focus: a shelf card while the shelf is focused, else an item of `items`.
    const Item* selected() const {
        if (shelfFocused()) return shelfSel < (int)shelf.size() ? &shelf[shelfSel] : nullptr;
        int s = sel();
        return s >= 0 && s < (int)items.size() ? &items[s] : nullptr;
    }
    bool shelfFocused() const { return !shelf.empty() && (gridLike() ? grid.bandSel : list.bandSel); }
    // Index into `shelf` picked this frame, or -1.
    int takeShelfActivated() {
        int r = shelfActivated_;
        shelfActivated_ = -1;
        return r;
    }
    bool atTop() const {
        if (shelfFocused()) return (gridLike() ? grid.bandRow : list.bandAt) == 0;
        return gridLike() ? grid.sel < grid.cols : list.sel == 0;
    }
    void scrollToTop();
    void draw(const Rect& r, const std::function<void(float y)>& header = nullptr);

private:
    void sync(const Rect& r);
    void drawShelf(const Rect& r, float y);
    float shelfCardW(const Rect& r) const;
    bool retry_ = false;
    Rect retryBtn_;
    int shelfActivated_ = -1;
    float shelfScroll_ = 0;
};

// ---------------------------------------------------------------- Menu

class Menu {
public:
    // checked >= 0 marks the current choice with a check (radio-style menus).
    void open(const std::string& title, const std::vector<std::string>& items, int sel = 0, int checked = -1);
    // Checklist: items with a checkbox (1 on, 0 off, -1 no box). Picking an item returns its
    // index and leaves the menu open; the caller flips the box with setCheck.
    void openChecklist(const std::string& title, const std::vector<std::string>& items, const std::vector<int>& checks,
                       int sel = 0);
    void setCheck(int i, bool on) {
        if (i >= 0 && i < (int)checks_.size() && checks_[i] >= 0) checks_[i] = on ? 1 : 0;
    }
    // Leading icons, one per item (call after open).
    void setIcons(const std::vector<Icon>& icons) { icons_ = icons; }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    bool isChecklist() const { return open_ && checklist_; }
    // Returns the chosen index, -2 if cancelled, -1 otherwise.
    int update(Input& in);
    void draw();
    int selected() const { return sel_; }

private:
    bool open_ = false;
    std::string title_;
    std::vector<std::string> items_;
    std::vector<Icon> icons_;
    std::vector<int> checks_;
    bool checklist_ = false;
    int sel_ = 0, checked_ = -1, first_ = 0;
    Rect box_;
    float itemY0_ = 0;
};

}  // namespace vt
