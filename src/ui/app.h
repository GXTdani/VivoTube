// Application shell: services, the top bar, guide rail and guide drawer, per-tab screen stacks,
// focus zones, menus and snackbars.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/google.h"
#include "../core/innertube.h"
#include "../core/store.h"
#include "../core/update.h"
#include "../vita/ime.h"
#include "../vita/input.h"
#include "../vita/threads.h"
#include "../vita/thumbs.h"
#include "gfx.h"
#include "widgets.h"

#ifndef VT_VERSION
#define VT_VERSION "1.0"
#endif

namespace vt {

constexpr float TOPBAR_H = 52;
constexpr float RAIL_W = 72;
// Content area below the top bar and right of the guide rail.
constexpr Rect CONTENT{RAIL_W, TOPBAR_H, SCREEN_W - RAIL_W, SCREEN_H - TOPBAR_H};
// Mini player card in the bottom-right corner: the video, then a bar with title and buttons.
constexpr float MINI_W = 304, MINI_VIDEO_H = 171, MINI_BAR_H = 48;
constexpr Rect MINI_RECT{SCREEN_W - 12 - MINI_W, SCREEN_H - 12 - MINI_VIDEO_H - MINI_BAR_H, MINI_W,
                         MINI_VIDEO_H + MINI_BAR_H};
// Centres of the mini player's play/pause and close buttons.
constexpr float MINI_BTN_Y = MINI_RECT.y + MINI_VIDEO_H + MINI_BAR_H / 2;
constexpr float MINI_CLOSE_X = MINI_RECT.x + MINI_W - 22, MINI_PLAY_X = MINI_CLOSE_X - 38;

enum Tab { TAB_HOME = 0, TAB_SHORTS = 1, TAB_SUBS = 2, TAB_YOU = 3, TAB_COUNT = 4 };

class Screen {
public:
    virtual ~Screen() = default;
    // Called only while the content area has focus. Screens consume the d-pad directions they
    // use; an unconsumed Up moves focus to the top bar and an unconsumed Left to the guide rail.
    virtual void update(Input& in) = 0;
    virtual void draw() = 0;
    // Fullscreen screens (the watch page) hide the top bar and guide rail.
    virtual bool fullscreen() const { return false; }
    virtual void onShow() {}  // became the top screen again
    virtual void onHide() {}  // another screen was pushed on top
    virtual bool isWatch() const { return false; }
    // Text shown in the top bar's search box (search results show their query).
    virtual std::string searchQuery() const { return ""; }
    // The search page replaces itself with results instead of stacking under them.
    virtual bool isSearchPage() const { return false; }
    // The tab of this (root) screen was picked again: scroll back to the top and return true, or
    // return false when already there, and the app calls refresh().
    virtual bool scrollToTop() { return false; }
    // Reloads the page (pull-to-refresh, or picking the tab again at the top).
    virtual void refresh() {}
    // Screens with a video player of their own (watch page, Shorts): opening one closes the
    // mini player.
    virtual bool playsVideo() const { return false; }

    // Mini player (watch page): whether leaving may shrink it into the corner instead of closing
    // it, and the hooks the app uses while it is small.
    virtual bool canMinimize() const { return false; }
    virtual void setMinimized(bool) {}
    virtual void tickMinimized() {}  // every frame while small (playback, history)
    virtual void drawMini(const Rect&, bool /*focused*/) {}
    virtual void togglePlay() {}
    virtual std::string videoId() const { return ""; }

protected:
    // Runs work() on a worker thread, then done(result) on the main thread, unless this
    // screen has been closed in the meantime. Background work waits for other queued work.
    template <class T, class W, class D>
    void async(W work, D done, bool background = false);
    // Wraps a callback so it does nothing once this screen is gone.
    template <class F>
    auto guarded(F f) {
        std::weak_ptr<bool> alive = alive_;
        return [alive, f](auto&&... args) mutable {
            if (alive.lock()) f(std::forward<decltype(args)>(args)...);
        };
    }
    // Whether the content area (rather than the top bar or rail) has d-pad focus.
    static bool active();

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// One entry of an option menu: label, leading icon and action.
struct MenuAction {
    std::string label;
    Icon icon;
    std::function<void()> run;
};

class App {
public:
    bool init(const std::string& dataDir);
    void run();
    void shutdown();

    InnerTube yt;
    Store store;
    GoogleAccount account;
    JobPool jobs;
    ThumbCache thumbs;
    Input input;
    Ime ime;
    std::string dataDir;

    // Navigation (all main thread).
    void push(std::unique_ptr<Screen> s);
    void pop();
    void replaceTop(std::unique_ptr<Screen> s);
    void switchTab(int tab);
    int tab() const { return tab_; }
    Screen* top();
    size_t depth() const { return stacks_[tab_].size(); }

    void openItem(const Item& it, const std::vector<Item>& queue = {}, int queueIndex = -1);
    void openVideo(const Item& it, const std::vector<Item>& queue = {}, int queueIndex = -1);
    // Opens a Short in the Shorts player (with the given list to swipe through).
    void openShort(const Item& it, const std::vector<Item>& list = {}, int index = -1);
    void openChannel(const std::string& id, const std::string& name);
    void openPlaylist(const std::string& id, const std::string& title);
    void openLocalPlaylist(const std::string& id);
    void openLiked();
    // The search page (recent searches) with the keyboard open.
    void openSearch();
    void openResults(const std::string& query);
    void openSettings();
    void openSignIn();
    void openGuide();

    // Modal option menu; cb receives the chosen index (not called when cancelled). checked >= 0
    // shows a check mark next to the current choice.
    void showMenu(const std::string& title, const std::vector<std::string>& items, std::function<void(int)> cb,
                  int sel = 0, int checked = -1, const std::vector<Icon>& icons = {});
    void showActions(const std::string& title, const std::vector<MenuAction>& actions);
    // Checklist menu: cb(index) runs for every pick and returns the item's new check state.
    void showChecklist(const std::string& title, const std::vector<std::string>& items, const std::vector<int>& checks,
                       std::function<int(int)> cb, const std::vector<Icon>& icons = {});
    // Standard options for an item (Watch Later, playlists, share, channel). extra entries come first.
    void itemMenu(const Item& it, std::vector<std::pair<std::string, std::function<void()>>> extra = {});
    void confirm(const std::string& question, const std::string& yesLabel, std::function<void()> yes);
    // System keyboard; cb runs with the text when the user confirms; cancel runs otherwise.
    void textInput(const std::string& title, const std::string& initial, std::function<void(const std::string&)> cb,
                   std::function<void()> cancel = nullptr);
    void toast(const std::string& msg);

    // Keeps the system from dimming the screen or suspending (call every frame while playing).
    void keepAwake() { awake_ = true; }
    bool contentFocused() const { return zone_ == Zone::Content && !guideOpen_; }
    // Mini player: shrink the watch page on top of the current tab into it, bring it back
    // full size, or close it.
    void minimizeTop(bool showBelow);
    void expandMini();
    void closeMini();
    bool hasMini() const { return mini_ != nullptr; }
    void focusContent() { zone_ = Zone::Content; }

    // Text helpers that depend on the system button layout.
    std::string confirmGlyph() const;
    std::string cancelGlyph() const;

    void applyLocale();
    void toggleSubscription(const std::string& id, const std::string& name, const std::string& avatar);
    // Like (1), dislike (-1) or clear (0); synced to the account when signed in.
    void rateVideo(const Item& it, int rating);
    // "Save to..." checklist: Watch later, local playlists and New playlist.
    void saveToPlaylist(const Item& it);
    void share(const std::string& title, const std::string& url);
    void shareItem(const Item& it);
    // Pulls the account's subscriptions into the local list.
    void syncSubscriptions(bool quiet);
    void accountMenu();
    // Looks for a newer release on GitHub (manual: also report "up to date" and ignore Skip).
    void checkForUpdates(bool manual);
    // Bumped when the account signs in or out (screens refresh account-dependent parts).
    uint32_t accountGen = 0;

private:
    enum class Zone { Content, TopBar, Rail, Mini };
    enum TopItem { TOP_BACK, TOP_GUIDE, TOP_SEARCH, TOP_MORE, TOP_ACCOUNT };

    void updateShell(Screen* s);
    void handleShellTaps();
    void drawTopBar();
    void drawRail();
    void drawToast();
    void updateGuide();
    void drawGuide();
    void buildGuide();
    void topAction(TopItem t);
    void moreMenu();
    std::vector<TopItem> topItems() const;
    float topX(TopItem t) const;
    std::unique_ptr<Screen> makeRoot(int tab);

    std::vector<std::unique_ptr<Screen>> stacks_[TAB_COUNT];
    std::vector<std::unique_ptr<Screen>> graveyard_;  // destroyed after the frame
    std::unique_ptr<Screen> mini_;                     // the minimized watch page
    int tab_ = 0;
    Zone zone_ = Zone::Content;
    int railSel_ = 0;
    TopItem topSel_ = TOP_SEARCH;
    Menu menu_;
    std::function<void(int)> menuCb_;
    std::function<int(int)> checkCb_;
    std::function<void(const std::string&)> imeCb_;
    std::function<void()> imeCancel_;
    std::string toast_;
    double toastStart_ = 0, toastUntil_ = 0;
    bool awake_ = false;
    bool quit_ = false;

    // Guide drawer (opened with the menu button in the top bar).
    struct GuideEntry {
        enum Kind { Link, Header, Divider, Channel } kind = Link;
        std::string label, id, avatar;
        Icon icon = Icon::Home;
        std::function<void()> run;
        bool current = false;
    };
    bool guideOpen_ = false;
    double guideT_ = 0;  // open animation start
    std::vector<GuideEntry> guide_;
    ListView guideList_;
    bool updateChecked_ = false;
};

extern App* g_app;

template <class T, class W, class D>
void Screen::async(W work, D done, bool background) {
    auto result = std::make_shared<T>();
    std::weak_ptr<bool> alive = alive_;
    g_app->jobs.post([result, work]() mutable { *result = work(); },
                     [result, alive, done]() mutable {
                         if (alive.lock()) done(*result);
                     },
                     background);
}

inline bool Screen::active() { return g_app->contentFocused(); }

// Screens (screens.cpp / watch.cpp / shorts.cpp / account.cpp).
std::unique_ptr<Screen> make_home_screen();
std::unique_ptr<Screen> make_shorts_screen(const std::vector<Item>& list = {}, int index = -1);
std::unique_ptr<Screen> make_subscriptions_screen();
std::unique_ptr<Screen> make_you_screen();
std::unique_ptr<Screen> make_search_screen(const std::string& initial);
std::unique_ptr<Screen> make_results_screen(const std::string& query);
std::unique_ptr<Screen> make_history_screen();
std::unique_ptr<Screen> make_watch_later_screen();
std::unique_ptr<Screen> make_liked_screen();
std::unique_ptr<Screen> make_local_playlist_screen(const std::string& id);
std::unique_ptr<Screen> make_account_playlist_screen(const std::string& id, const std::string& title);
std::unique_ptr<Screen> make_account_playlists_screen();
std::unique_ptr<Screen> make_playlists_screen();
std::unique_ptr<Screen> make_channels_screen();
std::unique_ptr<Screen> make_settings_screen();
std::unique_ptr<Screen> make_channel_screen(const std::string& id, const std::string& name);
std::unique_ptr<Screen> make_playlist_screen(const std::string& id, const std::string& title);
std::unique_ptr<Screen> make_watch_screen(const Item& it, const std::vector<Item>& queue, int queueIndex);
std::unique_ptr<Screen> make_signin_screen();
std::unique_ptr<Screen> make_update_screen(const Release& r);

}  // namespace vt
