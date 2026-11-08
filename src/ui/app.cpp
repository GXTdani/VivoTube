#include "app.h"

#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <algorithm>
#include <cmath>

#include "../core/util.h"

namespace vt {

App* g_app = nullptr;

namespace {

struct RailEntry {
    const char* label;
    Icon icon, iconOn;
};
const RailEntry kRail[TAB_COUNT] = {{"Home", Icon::Home, Icon::HomeFilled},
                                    {"Shorts", Icon::Shorts, Icon::ShortsFilled},
                                    {"Subscriptions", Icon::Subscriptions, Icon::SubscriptionsFilled},
                                    {"You", Icon::You, Icon::YouFilled}};

constexpr float kRailItemH = 74;
float rail_y(int i) { return TOPBAR_H + 2 + i * kRailItemH; }

// Top bar geometry.
constexpr float kBarCy = 26, kIconBtnR = 20;
constexpr float kSearchX = 318, kSearchY = 8, kSearchW = 312, kSearchH = 36, kSearchBtnW = 58;
constexpr float kSignInW = 104, kSignInH = 34;
constexpr float kGuideW = 240;

}  // namespace

bool App::init(const std::string& dir) {
    dataDir = dir;
    store.setDir(dir);
    store.load();
    account.init(dir);
    std::string err;
    if (!yt.loadProfiles("app0:assets/clients.json", dir + "/clients.json", &err)) VT_LOG("profiles: %s", err.c_str());
    applyLocale();
    input.init();
    jobs.start(3, 512 * 1024);
    thumbs.start();
    stacks_[0].push_back(makeRoot(0));
    return true;
}

void App::applyLocale() { yt.setLocale(store.settings.hl, store.settings.gl); }

void App::shutdown() {
    for (auto& st : stacks_) {
        for (auto it = st.rbegin(); it != st.rend(); ++it) (*it)->onHide();
        st.clear();
    }
    graveyard_.clear();
    thumbs.stop();
    jobs.stop();
}

std::unique_ptr<Screen> App::makeRoot(int t) {
    switch (t) {
        case TAB_HOME: return make_home_screen();
        case TAB_SHORTS: return make_shorts_screen();
        case TAB_SUBS: return make_subscriptions_screen();
        default: return make_you_screen();
    }
}

Screen* App::top() { return stacks_[tab_].empty() ? nullptr : stacks_[tab_].back().get(); }

void App::push(std::unique_ptr<Screen> s) {
    if (Screen* t = top()) t->onHide();
    stacks_[tab_].push_back(std::move(s));
    zone_ = Zone::Content;
}

void App::pop() {
    auto& st = stacks_[tab_];
    if (st.size() <= 1) return;
    st.back()->onHide();
    graveyard_.push_back(std::move(st.back()));
    st.pop_back();
    st.back()->onShow();
    zone_ = Zone::Content;
}

void App::replaceTop(std::unique_ptr<Screen> s) {
    auto& st = stacks_[tab_];
    if (!st.empty()) {
        st.back()->onHide();
        graveyard_.push_back(std::move(st.back()));
        st.pop_back();
    }
    st.push_back(std::move(s));
    zone_ = Zone::Content;
}

void App::switchTab(int t) {
    zone_ = Zone::Content;
    if (t == tab_) {
        // Re-selecting the current tab returns to its root.
        auto& st = stacks_[tab_];
        if (st.size() > 1) {
            st.back()->onHide();
            while (st.size() > 1) {
                graveyard_.push_back(std::move(st.back()));
                st.pop_back();
            }
            st.back()->onShow();
        }
        return;
    }
    if (Screen* cur = top()) cur->onHide();
    tab_ = t;
    railSel_ = t;
    if (stacks_[t].empty()) stacks_[t].push_back(makeRoot(t));
    else top()->onShow();
}

void App::openItem(const Item& it, const std::vector<Item>& queue, int queueIndex) {
    switch (it.type) {
        case ItemType::Video:
            if (it.shorts) openShort(it, queue, queueIndex);
            else openVideo(it, queue, queueIndex);
            break;
        case ItemType::Playlist: openPlaylist(it.id, it.title); break;
        case ItemType::Channel: openChannel(it.id, it.title); break;
    }
}

void App::openVideo(const Item& it, const std::vector<Item>& queue, int queueIndex) {
    if (it.id.empty()) return;
    if (it.live) {
        toast("Live streams aren't supported yet");
        return;
    }
    auto s = make_watch_screen(it, queue, queueIndex);
    Screen* t = top();
    if (t && t->isWatch()) replaceTop(std::move(s));
    else push(std::move(s));
}

void App::openShort(const Item& it, const std::vector<Item>& list, int index) {
    if (it.id.empty()) return;
    std::vector<Item> l = list;
    int i = index;
    if (l.empty() || i < 0 || i >= (int)l.size() || l[i].id != it.id) {
        l = {it};
        i = 0;
    }
    push(make_shorts_screen(l, i));
}

void App::openChannel(const std::string& id, const std::string& name) {
    if (id.empty()) return;
    push(make_channel_screen(id, name));
}

void App::openPlaylist(const std::string& id, const std::string& title) {
    if (id.empty()) return;
    if (id.compare(0, 2, "VT") == 0) {
        openLocalPlaylist(id);
        return;
    }
    push(make_playlist_screen(id, title));
}

void App::openLocalPlaylist(const std::string& id) {
    if (!store.playlist(id)) return;
    push(make_local_playlist_screen(id));
}

void App::openLiked() { push(make_liked_screen()); }

void App::openSearch() {
    Screen* t = top();
    push(make_search_screen(t ? t->searchQuery() : ""));
}

void App::openResults(const std::string& query) {
    store.addSearch(query);
    store.saveSearches();
    // The search page replaces itself with the results, so Back returns to where search started.
    Screen* t = top();
    auto s = make_results_screen(query);
    if (t && t->isSearchPage()) replaceTop(std::move(s));
    else push(std::move(s));
}

void App::openSettings() { push(make_settings_screen()); }

void App::openSignIn() { push(make_signin_screen()); }

void App::showMenu(const std::string& title, const std::vector<std::string>& items, std::function<void(int)> cb,
                   int sel, int checked, const std::vector<Icon>& icons) {
    if (items.empty()) return;
    menu_.open(title, items, sel, checked);
    if (!icons.empty()) menu_.setIcons(icons);
    menuCb_ = std::move(cb);
    checkCb_ = nullptr;
}

void App::showActions(const std::string& title, const std::vector<MenuAction>& actions) {
    auto acts = std::make_shared<std::vector<MenuAction>>(actions);
    std::vector<std::string> labels;
    std::vector<Icon> icons;
    for (auto& a : actions) {
        labels.push_back(a.label);
        icons.push_back(a.icon);
    }
    showMenu(title, labels, [acts](int i) {
        if (i >= 0 && i < (int)acts->size() && (*acts)[i].run) (*acts)[i].run();
    }, 0, -1, icons);
}

void App::showChecklist(const std::string& title, const std::vector<std::string>& items,
                        const std::vector<int>& checks, std::function<int(int)> cb, const std::vector<Icon>& icons) {
    if (items.empty()) return;
    menu_.openChecklist(title, items, checks);
    if (!icons.empty()) menu_.setIcons(icons);
    menuCb_ = nullptr;
    checkCb_ = std::move(cb);
}

void App::toggleSubscription(const std::string& id, const std::string& name, const std::string& avatar) {
    if (id.empty()) return;
    bool sub = !store.isSubscribed(id);
    if (!sub) {
        store.unsubscribe(id);
        toast("Subscription removed");
    } else {
        store.subscribe(id, name, avatar);
        toast("Subscribed to " + name);
    }
    store.saveSubs();
    if (account.signedIn()) {
        auto err = std::make_shared<std::string>();
        auto ok = std::make_shared<bool>(false);
        jobs.post([this, id, sub, err, ok] { *ok = sub ? account.subscribe(id, err.get()) : account.unsubscribe(id, err.get()); },
                  [this, err, ok] {
                      if (!*ok) toast("Couldn't update your YouTube account: " + *err);
                  });
    }
}

void App::rateVideo(const Item& it, int rating) {
    int before = store.rating(it.id);
    store.setRating(it, rating);
    store.saveRatings();
    if (rating == 1) toast("Added to Liked videos");
    else if (rating == -1) toast("Disliked");
    else if (before == 1) toast("Removed from Liked videos");
    else toast("Dislike removed");
    if (account.signedIn()) {
        std::string id = it.id;
        auto err = std::make_shared<std::string>();
        auto ok = std::make_shared<bool>(false);
        jobs.post([this, id, rating, err, ok] { *ok = account.rate(id, rating, err.get()); },
                  [this, err, ok] {
                      if (!*ok) toast("Couldn't sync the rating: " + *err);
                  });
    }
}

void App::saveToPlaylist(const Item& it) {
    if (it.type != ItemType::Video || it.id.empty()) return;
    std::vector<std::string> labels{"Watch later"};
    std::vector<int> checks{store.inWatchLater(it.id) ? 1 : 0};
    std::vector<Icon> icons{Icon::WatchLater};
    auto ids = std::make_shared<std::vector<std::string>>();
    for (auto& p : store.playlists) {
        labels.push_back(p.title);
        checks.push_back(store.inPlaylist(p.id, it.id) ? 1 : 0);
        icons.push_back(Icon::Lock);
        ids->push_back(p.id);
    }
    bool yt = account.signedIn();
    if (yt) {
        labels.push_back("Save to a YouTube playlist");
        checks.push_back(-1);
        icons.push_back(Icon::Playlist);
    }
    labels.push_back("New playlist");
    checks.push_back(-1);
    icons.push_back(Icon::Plus);
    Item copy = it;
    int n = (int)ids->size();
    showChecklist("Save video to...", labels, checks, [this, copy, ids, n, yt](int i) -> int {
        if (i == 0) {
            store.toggleWatchLater(copy);
            store.saveWatchLater();
            bool on = store.inWatchLater(copy.id);
            toast(on ? "Saved to Watch later" : "Removed from Watch later");
            return on ? 1 : 0;
        }
        if (i >= 1 && i <= n) {
            const std::string& pid = (*ids)[i - 1];
            LocalPlaylist* p = store.playlist(pid);
            if (!p) return -1;
            std::string title = p->title;
            bool added = store.togglePlaylistItem(pid, copy);
            store.savePlaylists();
            toast((added ? "Saved to " : "Removed from ") + title);
            return added ? 1 : 0;
        }
        if (yt && i == n + 1) {
            auto lists = std::make_shared<std::vector<Item>>();
            auto err = std::make_shared<std::string>();
            toast("Loading your playlists...");
            jobs.post([this, lists, err] { account.myPlaylists(*lists, err.get()); },
                      [this, lists, err, copy] {
                          if (lists->empty()) {
                              toast(err->empty() ? "Your account has no playlists" : *err);
                              return;
                          }
                          std::vector<std::string> names;
                          for (auto& l : *lists) names.push_back(l.title);
                          showMenu("Save to YouTube playlist", names, [this, lists, copy](int k) {
                              std::string pid = (*lists)[k].id, title = (*lists)[k].title;
                              auto e = std::make_shared<std::string>();
                              auto ok = std::make_shared<bool>(false);
                              jobs.post([this, pid, copy, e, ok] { *ok = account.addToPlaylist(pid, copy.id, e.get()); },
                                        [this, e, ok, title] { toast(*ok ? "Saved to " + title : "Couldn't save: " + *e); });
                          });
                      });
            return -2;
        }
        // New playlist.
        textInput("New playlist", "", [this, copy](const std::string& name) {
            std::string t = trim(name);
            if (t.empty()) return;
            std::string pid = store.createPlaylist(t);
            store.togglePlaylistItem(pid, copy);
            store.savePlaylists();
            toast("Saved to " + t);
        });
        return -2;
    }, icons);
}

void App::share(const std::string& title, const std::string& url) {
    (void)title;
    showMenu("Share\n" + url, {"Close"}, nullptr, 0, -1, {Icon::Close});
}

void App::shareItem(const Item& it) {
    std::string url;
    switch (it.type) {
        case ItemType::Video: url = it.shorts ? "https://youtube.com/shorts/" + it.id : "https://youtu.be/" + it.id; break;
        case ItemType::Channel: url = "https://www.youtube.com/channel/" + it.id; break;
        case ItemType::Playlist: url = "https://www.youtube.com/playlist?list=" + it.id; break;
    }
    share(it.title, url);
}

void App::itemMenu(const Item& it, std::vector<std::pair<std::string, std::function<void()>>> extra) {
    std::vector<MenuAction> acts;
    for (auto& e : extra) acts.push_back({e.first, Icon::Trash, e.second});
    Item copy = it;
    if (it.type == ItemType::Video) {
        bool wl = store.inWatchLater(it.id);
        acts.push_back({wl ? "Remove from Watch later" : "Save to Watch later", Icon::WatchLater, [this, copy, wl] {
                            store.toggleWatchLater(copy);
                            store.saveWatchLater();
                            toast(wl ? "Removed from Watch later" : "Saved to Watch later");
                        }});
        acts.push_back({"Save to playlist", Icon::Save, [this, copy] { saveToPlaylist(copy); }});
        acts.push_back({"Share", Icon::Share, [this, copy] { shareItem(copy); }});
        if (!it.channelId.empty()) {
            acts.push_back({"Go to channel", Icon::Account, [this, copy] { openChannel(copy.channelId, copy.channel); }});
            bool sub = store.isSubscribed(it.channelId);
            acts.push_back({sub ? "Unsubscribe from channel" : "Subscribe to channel", Icon::Subscriptions,
                            [this, copy] { toggleSubscription(copy.channelId, copy.channel, copy.avatar); }});
        }
    } else if (it.type == ItemType::Channel) {
        acts.push_back({"Go to channel", Icon::Account, [this, copy] { openChannel(copy.id, copy.title); }});
        bool sub = store.isSubscribed(it.id);
        acts.push_back({sub ? "Unsubscribe" : "Subscribe", Icon::Subscriptions,
                        [this, copy] { toggleSubscription(copy.id, copy.title, copy.thumb); }});
        acts.push_back({"Share", Icon::Share, [this, copy] { shareItem(copy); }});
    } else {
        acts.push_back({"Open playlist", Icon::Playlist, [this, copy] { openPlaylist(copy.id, copy.title); }});
        if (!it.channelId.empty())
            acts.push_back({"Go to channel", Icon::Account, [this, copy] { openChannel(copy.channelId, copy.channel); }});
        acts.push_back({"Share", Icon::Share, [this, copy] { shareItem(copy); }});
    }
    showActions(it.title, acts);
}

void App::confirm(const std::string& question, const std::string& yesLabel, std::function<void()> yes) {
    showMenu(question, {yesLabel, "Cancel"}, [yes](int i) {
        if (i == 0 && yes) yes();
    }, 1);
}

void App::textInput(const std::string& title, const std::string& initial, std::function<void(const std::string&)> cb,
                    std::function<void()> cancel) {
    if (ime.active()) return;
    if (ime.open(title, initial, 128)) {
        imeCb_ = std::move(cb);
        imeCancel_ = std::move(cancel);
    } else {
        toast("Couldn't open the keyboard");
        if (cancel) cancel();
    }
}

void App::toast(const std::string& msg) {
    double now = monotonic_seconds();
    if (toast_.empty() || now > toastUntil_) toastStart_ = now;
    toast_ = msg;
    toastUntil_ = now + 3.0;
}

std::string App::confirmGlyph() const { return input.confirmIsCircle ? G_CIRCLE : G_CROSS; }
std::string App::cancelGlyph() const { return input.confirmIsCircle ? G_CROSS : G_CIRCLE; }

// ---------------------------------------------------------------- account and updates

void App::syncSubscriptions(bool quiet) {
    if (!account.signedIn()) return;
    if (!quiet) toast("Syncing subscriptions...");
    auto subs = std::make_shared<std::vector<Subscription>>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    jobs.post([this, subs, err, ok] { *ok = account.mySubscriptions(*subs, err.get()); },
              [this, subs, err, ok, quiet] {
                  if (!*ok) {
                      if (!quiet) toast("Couldn't load subscriptions: " + *err);
                      return;
                  }
                  int added = 0;
                  for (auto& s : *subs) {
                      if (store.isSubscribed(s.id)) continue;
                      store.subscribe(s.id, s.name, s.avatar);
                      added++;
                  }
                  if (added) store.saveSubs();
                  if (!quiet || added)
                      toast(added ? str_printf("Added %d subscription%s from your account", added, added == 1 ? "" : "s")
                                  : "Subscriptions are up to date");
              });
}

void App::accountMenu() {
    if (!account.signedIn()) {
        openSignIn();
        return;
    }
    GoogleProfile p = account.profile();
    std::string title = p.name + (p.handle.empty() ? "" : "\n" + p.handle);
    std::vector<MenuAction> acts;
    if (!p.channelId.empty())
        acts.push_back({"Your channel", Icon::Account, [this, p] { openChannel(p.channelId, p.name); }});
    acts.push_back({"Your playlists", Icon::Playlist, [this] { push(make_account_playlists_screen()); }});
    acts.push_back({"Liked videos", Icon::ThumbUp, [this] { openLiked(); }});
    acts.push_back({"Sync subscriptions", Icon::Subscriptions, [this] { syncSubscriptions(false); }});
    acts.push_back({"Sign out", Icon::SignOut, [this] {
                        confirm("Sign out of " + account.profile().name + "?", "Sign out", [this] {
                            jobs.post([this] { account.signOut(); }, [this] {
                                accountGen++;
                                toast("Signed out");
                            });
                        });
                    }});
    showActions(title, acts);
}

void App::checkForUpdates(bool manual) {
    if (manual) toast("Checking for updates...");
    auto rel = std::make_shared<Release>();
    jobs.post([rel] { *rel = fetch_latest_release(kUpdateRepo, VT_VERSION); },
              [this, rel, manual] {
                  if (!rel->ok()) {
                      VT_LOG("update check: %s", rel->error.c_str());
                      if (manual) toast("Couldn't check for updates: " + rel->error);
                      return;
                  }
                  bool newer = compare_versions(rel->version, VT_VERSION) > 0;
                  if (!newer) {
                      if (manual) toast("VivoTube " VT_VERSION " is the latest version");
                      return;
                  }
                  if (!manual && rel->version == store.settings.skipVersion) return;
                  if (Screen* t = top())
                      if (t->isWatch() && !manual) return;  // don't interrupt playback
                  push(make_update_screen(*rel));
              });
}

// ---------------------------------------------------------------- guide drawer

void App::buildGuide() {
    guide_.clear();
    auto link = [&](const std::string& label, Icon icon, std::function<void()> run, bool current = false) {
        GuideEntry e;
        e.label = label;
        e.icon = icon;
        e.run = std::move(run);
        e.current = current;
        guide_.push_back(e);
    };
    auto header = [&](const std::string& label, std::function<void()> run) {
        GuideEntry e;
        e.kind = GuideEntry::Header;
        e.label = label;
        e.run = std::move(run);
        guide_.push_back(e);
    };
    auto divider = [&] {
        GuideEntry e;
        e.kind = GuideEntry::Divider;
        guide_.push_back(e);
    };
    bool root = stacks_[tab_].size() == 1;
    link("Home", tab_ == TAB_HOME && root ? Icon::HomeFilled : Icon::Home, [this] { switchTab(TAB_HOME); },
         tab_ == TAB_HOME && root);
    link("Shorts", tab_ == TAB_SHORTS && root ? Icon::ShortsFilled : Icon::Shorts, [this] { switchTab(TAB_SHORTS); },
         tab_ == TAB_SHORTS && root);
    link("Subscriptions", tab_ == TAB_SUBS && root ? Icon::SubscriptionsFilled : Icon::Subscriptions,
         [this] { switchTab(TAB_SUBS); }, tab_ == TAB_SUBS && root);
    divider();
    header("You", [this] { switchTab(TAB_YOU); });
    link("History", Icon::History, [this] { push(make_history_screen()); });
    link("Playlists", Icon::Playlist, [this] { push(make_playlists_screen()); });
    link("Watch later", Icon::WatchLater, [this] { push(make_watch_later_screen()); });
    link("Liked videos", Icon::ThumbUp, [this] { openLiked(); });
    if (!store.subs.empty()) {
        divider();
        header("Subscriptions", [this] { push(make_channels_screen()); });
        size_t n = std::min<size_t>(store.subs.size(), 12);
        for (size_t i = 0; i < n; i++) {
            GuideEntry e;
            e.kind = GuideEntry::Channel;
            e.label = store.subs[i].name;
            e.id = store.subs[i].id;
            e.avatar = store.subs[i].avatar;
            std::string id = e.id, name = e.label;
            e.run = [this, id, name] { openChannel(id, name); };
            guide_.push_back(e);
        }
        if (store.subs.size() > n) link("All subscriptions", Icon::ChevronDown, [this] { push(make_channels_screen()); });
    }
    divider();
    link("Settings", Icon::Settings, [this] { openSettings(); });
    link("Check for updates", Icon::Download, [this] { checkForUpdates(true); });
    if (account.signedIn()) link("Sign out", Icon::SignOut, [this] { accountMenu(); });
    else link("Sign in", Icon::Account, [this] { openSignIn(); });
    guideList_.count = (int)guide_.size();
    guideList_.heights.clear();
    for (auto& e : guide_) guideList_.heights.push_back(e.kind == GuideEntry::Divider ? 17.0f : 40.0f);
}

void App::openGuide() {
    buildGuide();
    guideList_.reset();
    guideOpen_ = true;
    guideT_ = monotonic_seconds();
}

static Rect guide_rect() { return Rect{0, TOPBAR_H, kGuideW, SCREEN_H - TOPBAR_H - 30}; }

void App::updateGuide() {
    Input& in = input;
    Rect r = guide_rect();
    int before = guideList_.sel;
    guideList_.focused = true;
    int hit = guideList_.update(in, r);
    // Dividers are not selectable: keep going in the same direction.
    if (guideList_.sel != before && guide_[guideList_.sel].kind == GuideEntry::Divider) {
        int d = guideList_.sel > before ? 1 : -1;
        int s = guideList_.sel + d;
        guideList_.sel = s >= 0 && s < (int)guide_.size() ? s : before;
        guideList_.scrollToSelection(r);
    }
    if (hit >= 0 && guide_[hit].kind == GuideEntry::Divider) hit = -1;
    if (in.tapped) {
        // A tap outside the drawer closes it.
        in.consumeTap();
        if (in.tapX >= kGuideW) guideOpen_ = false;
    }
    if (hit >= 0) {
        guideOpen_ = false;
        zone_ = Zone::Content;
        auto run = guide_[hit].run;
        if (run) run();
    } else if (in.pressed(BTN_CANCEL) || in.pressed(BTN_RIGHT) || in.pressed(BTN_SELECT)) {
        guideOpen_ = false;
    }
    in.consume(0xFFFFFFFF);
}

void App::drawGuide() {
    float k = (float)std::min(1.0, (monotonic_seconds() - guideT_) / 0.16);
    k = 1 - (1 - k) * (1 - k);
    float ox = -kGuideW * (1 - k);
    g_gfx.rect(0, 0, SCREEN_W, SCREEN_H, rgba(0, 0, 0, (uint8_t)(150 * k)));
    g_gfx.rect(ox, 0, kGuideW, SCREEN_H, col::bg);
    // Drawer header: menu button and logo, where they sit in the top bar.
    draw_icon(Icon::Menu, ox + 26, kBarCy, 22, col::text);
    draw_brand_mark(ox + 50, 13, 26);
    g_gfx.textBold(ox + 82, 14, col::text, 0.9f, "VivoTube");
    Rect r = guide_rect();
    r.x += ox;
    guideList_.draw(r, [&](int i, const Rect& row, bool sel) {
        const GuideEntry& e = guide_[i];
        if (e.kind == GuideEntry::Divider) {
            g_gfx.rect(row.x + 12, row.y + 8, row.w - 24, 1, col::divider);
            return;
        }
        float x = row.x + 12, w = row.w - 24;
        uint32_t bg = sel ? col::raisedHi : e.current ? col::raised : col::bg;
        if (sel || e.current) g_gfx.roundRect(x, row.y + 2, w, row.h - 4, 10, bg);
        if (sel) g_gfx.roundFrame(x - 2, row.y, w + 4, row.h, 12, 2, col::focus);
        float ty = row.y + (row.h - g_gfx.lineHeight(0.78f)) / 2;
        if (e.kind == GuideEntry::Header) {
            g_gfx.text(x + 12, ty - 1, col::text, 0.86f, e.label, W_MEDIUM);
            float tw = (float)g_gfx.width(e.label, 0.86f, W_MEDIUM);
            draw_icon(Icon::ChevronRight, x + 12 + tw + 12, row.y + row.h / 2, 16, col::text, bg);
            return;
        }
        if (e.kind == GuideEntry::Channel) draw_avatar(e.avatar, e.label, x + 24, row.y + row.h / 2, 12, bg);
        else draw_icon(e.icon, x + 24, row.y + row.h / 2, 22, col::text, bg);
        g_gfx.text(x + 56, ty, col::text, 0.78f, g_gfx.ellipsize(e.label, w - 64, 0.78f), e.current ? W_MEDIUM : W_REGULAR);
    });
    g_gfx.text(ox + 24, SCREEN_H - 24, col::text3, 0.56f, "VivoTube " VT_VERSION G_DOT "Unofficial client");
}

// ---------------------------------------------------------------- shell input

std::vector<App::TopItem> App::topItems() const {
    std::vector<TopItem> v;
    if (stacks_[tab_].size() > 1) v.push_back(TOP_BACK);
    v.push_back(TOP_GUIDE);
    v.push_back(TOP_SEARCH);
    v.push_back(TOP_MORE);
    v.push_back(TOP_ACCOUNT);
    return v;
}

// Center x of an icon button (or left edge for the search box and sign-in pill).
float App::topX(TopItem t) const {
    bool back = stacks_[tab_].size() > 1;
    bool signedIn = account.signedIn();
    switch (t) {
        case TOP_BACK: return 26;
        case TOP_GUIDE: return back ? 68 : 26;
        case TOP_SEARCH: return kSearchX;
        case TOP_MORE: return signedIn ? SCREEN_W - 82 : SCREEN_W - 16 - kSignInW - 30;
        case TOP_ACCOUNT: return signedIn ? SCREEN_W - 34 : SCREEN_W - 16 - kSignInW;
    }
    return 0;
}

void App::topAction(TopItem t) {
    zone_ = Zone::Content;
    switch (t) {
        case TOP_BACK: pop(); break;
        case TOP_GUIDE: openGuide(); break;
        case TOP_SEARCH: openSearch(); break;
        case TOP_MORE: moreMenu(); break;
        case TOP_ACCOUNT: accountMenu(); break;
    }
}

void App::moreMenu() {
    std::vector<MenuAction> acts;
    acts.push_back({"Settings", Icon::Settings, [this] { openSettings(); }});
    acts.push_back({"Check for updates", Icon::Download, [this] { checkForUpdates(true); }});
    if (account.signedIn()) acts.push_back({"Your account", Icon::Account, [this] { accountMenu(); }});
    else acts.push_back({"Sign in", Icon::Account, [this] { openSignIn(); }});
    acts.push_back({"About VivoTube", Icon::Info, [this] {
                        showMenu("VivoTube " VT_VERSION "\nAn unofficial YouTube client for PS Vita. Not affiliated with "
                                 "or endorsed by YouTube or Google.",
                                 {"Close"}, nullptr);
                    }});
    showActions("", acts);
}

void App::handleShellTaps() {
    if (!input.tapped) return;
    float x = input.tapX, y = input.tapY;
    if (y < TOPBAR_H) {
        input.consumeTap();
        zone_ = Zone::Content;
        bool back = stacks_[tab_].size() > 1;
        if (back && x < 46) topAction(TOP_BACK);
        else if (x < topX(TOP_GUIDE) + 22) topAction(TOP_GUIDE);
        else if (x >= kSearchX - 10 && x < kSearchX + kSearchW + kSearchBtnW + 10) topAction(TOP_SEARCH);
        else if (std::fabs(x - topX(TOP_MORE)) < 22) topAction(TOP_MORE);
        else if (x >= topX(TOP_ACCOUNT) - 24) topAction(TOP_ACCOUNT);
        return;
    }
    if (x < RAIL_W) {
        input.consumeTap();
        for (int i = 0; i < TAB_COUNT; i++)
            if (y >= rail_y(i) && y < rail_y(i) + kRailItemH) switchTab(i);
        zone_ = Zone::Content;
    }
}

void App::updateShell(Screen* s) {
    Input& in = input;
    if (in.pressed(BTN_SELECT)) {
        in.consume(BTN_SELECT);
        openGuide();
        return;
    }
    if (zone_ == Zone::Rail) {
        if (in.repeat(BTN_DOWN) && railSel_ + 1 < TAB_COUNT) railSel_++;
        else if (in.repeat(BTN_UP)) {
            if (railSel_ > 0) railSel_--;
            else if (in.pressed(BTN_UP)) {
                zone_ = Zone::TopBar;
                topSel_ = topItems().front();
            }
        }
        if (in.pressed(BTN_CONFIRM)) switchTab(railSel_);
        else if (in.pressed(BTN_RIGHT) || in.pressed(BTN_CANCEL)) zone_ = Zone::Content;
        if (in.pressed(BTN_TRIANGLE)) openSearch();
        in.consume(0xFFFFFFFF);
        return;
    }
    if (zone_ == Zone::TopBar) {
        std::vector<TopItem> items = topItems();
        int idx = 0;
        for (size_t i = 0; i < items.size(); i++)
            if (items[i] == topSel_) idx = (int)i;
        if (in.repeat(BTN_RIGHT) && idx + 1 < (int)items.size()) topSel_ = items[idx + 1];
        if (in.repeat(BTN_LEFT)) {
            if (idx > 0) topSel_ = items[idx - 1];
            else if (in.pressed(BTN_LEFT)) {
                zone_ = Zone::Rail;
                railSel_ = tab_;
            }
        }
        if (in.pressed(BTN_DOWN) || in.pressed(BTN_CANCEL)) zone_ = Zone::Content;
        if (in.pressed(BTN_CONFIRM)) topAction(topSel_);
        else if (in.pressed(BTN_TRIANGLE)) openSearch();
        in.consume(0xFFFFFFFF);
        return;
    }
    // Content has focus: run the screen, then handle what it left unconsumed.
    s->update(in);
    if (top() != s) return;
    if (in.pressed(BTN_UP)) {
        zone_ = Zone::TopBar;
        topSel_ = TOP_SEARCH;
    } else if (in.pressed(BTN_LEFT)) {
        zone_ = Zone::Rail;
        railSel_ = tab_;
    } else if (in.pressed(BTN_TRIANGLE)) {
        openSearch();
    } else if (in.pressed(BTN_CANCEL)) {
        if (stacks_[tab_].size() > 1) pop();
        else if (tab_ != TAB_HOME) switchTab(TAB_HOME);
    }
}

// ---------------------------------------------------------------- shell drawing

static void icon_button(float cx, Icon icon, bool focused, float size = 22) {
    if (focused) g_gfx.circle(cx, kBarCy, kIconBtnR, col::raisedHi);
    draw_icon(icon, cx, kBarCy, size, col::text, focused ? col::raisedHi : col::bg);
    if (focused) g_gfx.ring(cx, kBarCy, kIconBtnR + 2, 2.5f, col::focus);
}

void App::drawTopBar() {
    g_gfx.rect(0, 0, SCREEN_W, TOPBAR_H, col::bg);
    bool barFocus = zone_ == Zone::TopBar;
    bool back = stacks_[tab_].size() > 1;
    if (back) icon_button(topX(TOP_BACK), Icon::Back, barFocus && topSel_ == TOP_BACK);
    icon_button(topX(TOP_GUIDE), Icon::Menu, barFocus && topSel_ == TOP_GUIDE);

    // VivoTube's own mark and wordmark, with an "unofficial" tag where a region code would sit.
    float x = topX(TOP_GUIDE) + 26;
    draw_brand_mark(x, 13, 26);
    g_gfx.text(x + 31, 13, col::text, 0.92f, "VivoTube", W_BOLD);
    float tagX = x + 32 + g_gfx.width("VivoTube", 0.92f, W_BOLD);
    g_gfx.text(tagX, 8, col::text2, 0.42f, "UNOFFICIAL");

    // Search box, then the separate search button.
    bool sfoc = barFocus && topSel_ == TOP_SEARCH;
    float sx = kSearchX, sy = kSearchY, sw = kSearchW, sh = kSearchH, r = sh / 2;
    uint32_t edge = sfoc ? rgba(28, 98, 185) : col::searchEdge;
    g_gfx.roundRect(sx, sy, sw + r, sh, r, edge);  // left half-pill with border
    g_gfx.roundRect(sx + 1, sy + 1, sw + r - 2, sh - 2, r - 1, col::searchBg);
    float bx = sx + sw;
    g_gfx.roundRect(bx, sy, kSearchBtnW, sh, r, col::searchEdge);  // button: square left, round right
    g_gfx.rect(bx, sy, r, sh, col::searchEdge);
    g_gfx.roundRect(bx + 1, sy + 1, kSearchBtnW - 2, sh - 2, r - 1, col::searchBtn);
    g_gfx.rect(bx + 1, sy + 1, r, sh - 2, col::searchBtn);
    draw_icon(Icon::Search, bx + kSearchBtnW / 2 - 2, sy + sh / 2, 21, col::text, col::searchBtn);
    Screen* s = top();
    std::string q = s ? s->searchQuery() : "";
    float ty = sy + (sh - g_gfx.lineHeight(0.8f)) / 2;
    if (q.empty()) {
        g_gfx.text(sx + 16, ty, col::text2, 0.8f, "Search");
        g_gfx.textRight(bx - 12, ty + 1, col::text3, 0.66f, G_TRIANGLE);
    } else {
        g_gfx.text(sx + 16, ty, col::text, 0.8f, g_gfx.ellipsize(q, sw - 32, 0.8f));
    }
    if (sfoc) g_gfx.roundFrame(sx - 4, sy - 4, sw + kSearchBtnW + 8, sh + 8, r + 4, 2.5f, col::focus);

    icon_button(topX(TOP_MORE), Icon::More, barFocus && topSel_ == TOP_MORE, 20);

    bool afoc = barFocus && topSel_ == TOP_ACCOUNT;
    if (account.signedIn()) {
        GoogleProfile p = account.profile();
        float cx = topX(TOP_ACCOUNT);
        draw_avatar(p.avatar, p.name, cx, kBarCy, 16);
        if (afoc) g_gfx.ring(cx, kBarCy, 21, 2.5f, col::focus);
    } else {
        // "Sign in" outline pill.
        float px = topX(TOP_ACCOUNT), py = kBarCy - kSignInH / 2;
        uint32_t fill = afoc ? rgba(38, 61, 82) : col::bg;
        g_gfx.roundRect(px, py, kSignInW, kSignInH, kSignInH / 2, rgba(255, 255, 255, 50));
        g_gfx.roundRect(px + 1, py + 1, kSignInW - 2, kSignInH - 2, kSignInH / 2 - 1, fill);
        draw_icon(Icon::Account, px + 22, kBarCy, 22, col::blue, fill);
        g_gfx.text(px + 40, kBarCy - g_gfx.lineHeight(0.76f) / 2, col::blue, 0.76f, "Sign in", W_MEDIUM);
        if (afoc) g_gfx.roundFrame(px - 4, py - 4, kSignInW + 8, kSignInH + 8, kSignInH / 2 + 4, 2.5f, col::focus);
    }
}

void App::drawRail() {
    g_gfx.rect(0, TOPBAR_H, RAIL_W, SCREEN_H - TOPBAR_H, col::bg);
    bool railFocus = zone_ == Zone::Rail;
    for (int i = 0; i < TAB_COUNT; i++) {
        float y = rail_y(i);
        bool cur = i == tab_;
        bool foc = railFocus && i == railSel_;
        uint32_t bg = foc ? col::raisedHi : col::bg;
        if (foc) {
            g_gfx.roundRect(4, y, RAIL_W - 8, kRailItemH - 6, 10, bg);
            g_gfx.roundFrame(1, y - 3, RAIL_W - 2, kRailItemH, 13, 2.5f, col::focus);
        }
        draw_icon(cur ? kRail[i].iconOn : kRail[i].icon, RAIL_W / 2, y + 26, 24, col::text, bg);
        const char* label = kRail[i].label;
        float s = 0.56f;
        g_gfx.textCentered(RAIL_W / 2, y + 46, col::text, s, g_gfx.ellipsize(label, RAIL_W - 4, s), W_REGULAR);
    }
}

void App::drawToast() {
    double now = monotonic_seconds();
    if (toast_.empty() || now > toastUntil_) return;
    double in = std::min(1.0, (now - toastStart_) / 0.18), out = std::min(1.0, (toastUntil_ - now) / 0.25);
    float k = (float)std::min(in, out);
    const float s = 0.74f;
    float w = std::min(620.0f, (float)g_gfx.width(toast_, s) + 48);
    float h = 44;
    float x = 16, y = SCREEN_H - 16 - h + (1 - k) * 24;
    g_gfx.roundRect(x, y + 2, w, h, 8, rgba(0, 0, 0, (uint8_t)(90 * k)));
    g_gfx.roundRect(x, y, w, h, 8, with_alpha(col::text, (uint8_t)(255 * k)));
    g_gfx.text(x + 24, y + (h - g_gfx.lineHeight(s)) / 2, with_alpha(col::onLight, (uint8_t)(255 * k)), s,
               g_gfx.ellipsize(toast_, w - 40, s));
}

// ---------------------------------------------------------------- main loop

void App::run() {
    vita2d_set_clear_color(col::bg);
    while (!quit_) {
        input.poll();
        g_gfx.time = monotonic_seconds();
        g_gfx.frameNo++;
        jobs.poll();
        thumbs.update();
        awake_ = false;
        if (!updateChecked_ && g_gfx.frameNo > 30) {
            // Startup work once the first frames are on screen.
            updateChecked_ = true;
            if (store.settings.checkUpdates) checkForUpdates(false);
            syncSubscriptions(true);
        }

        if (ime.active()) {
            std::string text;
            int r = ime.poll(text);
            if (r != 0) {
                input.settle(6);
                auto cb = std::move(imeCb_);
                auto cancel = std::move(imeCancel_);
                imeCb_ = nullptr;
                imeCancel_ = nullptr;
                if (r > 0 && cb) cb(text);
                else if (r < 0 && cancel) cancel();
            }
            input.consume(0xFFFFFFFF);
            input.consumeTap();
        } else if (menu_.isOpen()) {
            int r = menu_.update(input);
            if (checkCb_) {
                if (r >= 0) {
                    auto cb = checkCb_;
                    int st = cb(r);
                    if (st == -2) {
                        // The pick moved on (keyboard, another menu): close the checklist unless
                        // the callback already replaced it with another menu.
                        if (menu_.isChecklist()) {
                            menu_.close();
                            checkCb_ = nullptr;
                        }
                    } else if (st >= 0) {
                        menu_.setCheck(r, st == 1);
                    }
                } else if (r == -2) {
                    checkCb_ = nullptr;
                }
            } else if (r != -1) {
                auto cb = std::move(menuCb_);
                menuCb_ = nullptr;
                if (r >= 0 && cb) cb(r);
            }
        } else if (guideOpen_) {
            updateGuide();
        } else if (Screen* s = top()) {
            if (s->fullscreen()) {
                zone_ = Zone::Content;
                s->update(input);
                if (top() == s && input.pressed(BTN_CANCEL)) {
                    input.consume(BTN_CANCEL);
                    if (stacks_[tab_].size() > 1) pop();
                }
                if (top() == s && input.pressed(BTN_TRIANGLE)) openSearch();
            } else {
                handleShellTaps();
                if (top() == s) updateShell(s);
            }
        }

        g_gfx.beginFrame();
        vita2d_start_drawing();
        vita2d_clear_screen();
        Screen* s = top();
        bool full = s && s->fullscreen();
        if (s) s->draw();
        if (!full) {
            drawTopBar();
            drawRail();
        }
        if (guideOpen_) drawGuide();
        menu_.draw();
        drawToast();
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();

        graveyard_.clear();
        if (awake_) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }
}

}  // namespace vt
