// Browse screens: Home, Search, Subscriptions, You (library), History, Watch later, Settings,
// Channel and Playlist pages.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <unordered_set>

#include "../core/foryou.h"
#include "../core/util.h"
#include "app.h"

namespace vt {

namespace {

constexpr float kChipsY = CONTENT.y + 8;
constexpr float kChipsX = CONTENT.x + 16;

void page_title(float x, float y, const std::string& t) { g_gfx.textBold(x, y, col::text, 1.1f, t); }

std::string plural(int n, const char* one, const char* many) { return str_printf("%d %s", n, n == 1 ? one : many); }

std::string subs_key() {
    std::string k;
    for (auto& s : g_app->store.subs) k += s.id + ",";
    return k;
}

uint32_t g_shuffleSeed = 0x9E3779B9u;

// Whether YouTube personalises its Home feed for the account's token: -1 not tried yet this
// session, 0 no (VivoTube builds "For you" itself), 1 yes.
int g_youtubeHome = -1;

struct HomeFetch {
    Page page;
    bool personal = false;
};

HomeFetch youtube_home(const std::string& continuation) {
    HomeFetch r;
    std::string token, err;
    if (!g_app->account.accessToken(token, &err)) {
        r.page.error = err;
        return r;
    }
    r.page = g_app->yt.home(token, continuation, &r.personal);
    return r;
}

std::vector<Item> shuffled(std::vector<Item> v) {
    g_shuffleSeed ^= (uint32_t)(monotonic_seconds() * 1000.0);
    for (size_t i = v.size(); i > 1; i--) {
        g_shuffleSeed = g_shuffleSeed * 1664525u + 1013904223u;
        std::swap(v[i - 1], v[(g_shuffleSeed >> 8) % i]);
    }
    return v;
}

}  // namespace

// Fetches every subscribed channel's RSS feed. `each` runs on the main thread once per channel.
class LibraryScreen : public Screen {
protected:
    void fetchSubsRss(std::function<void(std::vector<Item>&, bool ok)> each) {
        for (const Subscription& s : g_app->store.subs) {
            std::string id = s.id, avatar = s.avatar;
            struct R {
                std::vector<Item> items;
                std::string err;
            };
            async<R>([id] {
                R r;
                r.items = g_app->yt.channelRss(id, &r.err);
                return r;
            }, [each, avatar](R& r) {
                std::vector<Item> keep;
                for (auto& it : r.items) {
                    if (it.duration.empty() && contains(it.title, "#shorts")) continue;
                    if (it.avatar.empty()) it.avatar = avatar;
                    keep.push_back(std::move(it));
                }
                each(keep, r.err.empty());
            }, true);
        }
    }

    static void mergeByDate(std::vector<Item>& into, std::vector<Item>& more, size_t cap) {
        for (auto& it : more) into.push_back(std::move(it));
        std::stable_sort(into.begin(), into.end(), [](const Item& a, const Item& b) { return a.time > b.time; });
        if (into.size() > cap) into.resize(cap);
    }

    // Keeps the highlighted item highlighted when a feed is rebuilt.
    static void reselect(Feed& f, const std::string& id) {
        if (id.empty()) return;
        for (size_t k = 0; k < f.items.size(); k++)
            if (f.items[k].id == id) f.setSel((int)k);
    }
};

// ================================================================ Home

class HomeScreen : public LibraryScreen {
public:
    HomeScreen() {
        for (int i = 0; i < kChips; i++) {
            Feed& f = feeds_[i];
            f.style = FeedStyle::Grid;
            f.grid.padTop = 6;
            // Recommendations leave out what the user hid; "Recently uploaded" is their own
            // subscriptions, so it shows everything.
            if (i != kRecent) f.hide = [](const Item& it) { return g_app->store.hidden(it); };
        }
        feeds_[0].emptyIcon = Icon::Home;
        feeds_[0].emptyTitle = "Try searching to get started";
        feeds_[0].emptyText = "Start watching videos and subscribing to channels to build a feed of videos you'll like.";
        Feed& r = feeds_[kRecent];
        r.emptyIcon = Icon::Subscriptions;
        r.emptyTitle = "No recent uploads";
        r.emptyText = "Subscribe to channels and their newest videos will show up here.";
        loadAll();
    }

    bool scrollToTop() override {
        Feed& f = feeds_[cur_];
        if (f.atVeryTop()) return false;
        chipsFocus_ = false;
        f.scrollToTop();
        return true;
    }

    void refresh() override {
        chipsFocus_ = false;
        if (cur_ == 0) {
            fyRot_++;
            loadAll(true);
        } else if (def(cur_).kind == K_RECENT) {
            loadSubs();
        } else {
            loadChip(cur_);
        }
    }

    void update(Input& in) override {
        if (g_app->accountGen != accountGen_) {
            // Signed in or out: the personal parts of Home change.
            likedLoaded_ = false;
            accountLiked_.clear();
            g_youtubeHome = -1;
            loadAll(true);
        }
        refreshAllState();

        // Chips: tap, shoulder buttons, or d-pad when focused.
        int t = chips_hit(in, names(), kChipsX, kChipsY);
        if (t >= 0) in.consumeTap();
        if (in.pressed(BTN_R)) t = (cur_ + 1) % kChips;
        if (in.pressed(BTN_L)) t = (cur_ + kChips - 1) % kChips;
        if (chipsFocus_) {
            if (in.repeat(BTN_RIGHT) && chipSel_ + 1 < kChips) {
                in.consume(BTN_RIGHT);
                chipSel_++;
            }
            if (in.repeat(BTN_LEFT) && chipSel_ > 0) {
                in.consume(BTN_LEFT);
                chipSel_--;
            }
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                t = chipSel_;
            }
            if (in.pressed(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                chipsFocus_ = false;
            }
        }
        if (t >= 0) select(t);

        Feed& f = feeds_[cur_];
        f.setFocused(!chipsFocus_);
        int a = f.update(in, grid());
        if (a >= 0) g_app->openItem(f.items[a]);
        int sa = f.takeShelfActivated();
        if (sa >= 0) g_app->openShort(f.shelf[sa], f.shelf, sa);
        if (!chipsFocus_ && in.pressed(BTN_UP)) {
            in.consume(BTN_UP);
            chipsFocus_ = true;
            chipSel_ = cur_;
        }
        if (f.takeRetry()) {
            if (cur_ == 0) loadAll(true);
            else if (def(cur_).kind == K_RECENT) loadSubs();
            else loadChip(cur_);
        }
        if (f.takeRefresh()) refresh();
        if (def(cur_).kind == K_SEARCH && f.wantsMore(grid())) loadChip(cur_, true);
        if (cur_ == 0 && mode_ != Mode::Mix && !personalBusy_ && f.wantsMore(grid())) loadMorePersonal();
        if (!chipsFocus_ && in.pressed(BTN_SQUARE) && f.selected()) g_app->itemMenu(*f.selected(), {}, cur_ != kRecent);
    }

    void hiddenChanged() override {
        for (Feed& f : feeds_) f.prune();
    }

    void onShow() override {
        if (subs_key() != loadedSubs_) loadSubs();
    }

    void draw() override {
        Feed& f = feeds_[cur_];
        f.setFocused(active() && !chipsFocus_);
        f.draw(grid());
        g_gfx.rect(CONTENT.x, CONTENT.y, CONTENT.w, 46, col::bg);
        draw_chips(names(), cur_, active() && chipsFocus_ ? chipSel_ : -1, kChipsX, kChipsY);
    }

private:
    enum Kind { K_ALL, K_TOPIC, K_SEARCH, K_RECENT };
    struct ChipDef {
        const char* name;
        Kind kind;
        const char* arg;  // topic channel ID or search query
    };
    static constexpr int kChips = 7, kRecent = 6;
    static const ChipDef& def(int i) {
        static const ChipDef defs[kChips] = {{"All", K_ALL, ""},
                                             {"Music", K_TOPIC, "UC-9-kyTW8ZkZNDHQJ6FgpwQ"},
                                             {"Gaming", K_SEARCH, "gaming"},
                                             {"News", K_TOPIC, "UCYfdidRxbB8Qhf0Nx7ioOYw"},
                                             {"Sports", K_TOPIC, "UCEgdi0XIXXZ-qJOFPf4JSKw"},
                                             {"Podcasts", K_SEARCH, "podcast"},
                                             {"Recently uploaded", K_RECENT, ""}};
        return defs[i];
    }
    static Rect grid() { return Rect{CONTENT.x, CONTENT.y + 46, CONTENT.w, CONTENT.h - 46}; }
    static std::vector<std::string> names() {
        std::vector<std::string> v;
        for (int i = 0; i < kChips; i++) v.push_back(def(i).name);
        return v;
    }

    void select(int t) {
        if (t == cur_) {
            feeds_[cur_].scrollToTop();
            return;
        }
        cur_ = t;
        chipSel_ = t;
        Feed& f = feeds_[cur_];
        Kind k = def(cur_).kind;
        if ((k == K_TOPIC || k == K_SEARCH) && f.items.empty() && !f.loading) loadChip(cur_);
    }

    void loadAll(bool force = false) {
        feeds_[0].reset();
        accountGen_ = g_app->accountGen;
        // Decide first whether "All" is personal, so the mixed feed doesn't flash up meanwhile.
        mode_ = g_app->account.signedIn() || hasSeeds() ? Mode::ForYou : Mode::Mix;
        for (int i = 0; i < kChips; i++)
            if (def(i).kind == K_TOPIC && (force || (feeds_[i].items.empty() && !feeds_[i].loading))) loadChip(i);
        loadSubs();
        loadShorts();
        if (mode_ != Mode::Mix) loadPersonal();
    }

    // ---- personal "All" feed

    std::vector<Item> likedForSeeds() const {
        std::vector<Item> v = accountLiked_;
        v.insert(v.end(), g_app->store.liked.begin(), g_app->store.liked.end());
        return v;
    }
    bool hasSeeds() const {
        size_t total = 0;
        for_you_seeds(g_app->store.history, likedForSeeds(), 0, 1, &total);
        return total > 0;
    }

    void loadPersonal() {
        uint32_t gen = feeds_[0].gen;
        fyPage_ = 0;
        fySecond_ = 0;
        fyEmpty_ = 0;
        fySeeds_.clear();
        personalBusy_ = true;
        if (g_app->account.signedIn() && g_youtubeHome != 0) {
            // YouTube's own recommendations, if it accepts the token (tried once per session).
            mode_ = Mode::YouTube;
            async<HomeFetch>([] { return youtube_home(""); }, [this, gen](HomeFetch& r) {
                if (gen != feeds_[0].gen) return;
                if (r.personal) {
                    g_youtubeHome = 1;
                    personalBusy_ = false;
                    feeds_[0].append(r.page);
                    return;
                }
                if (!starts_with(r.page.error, "network error")) g_youtubeHome = 0;
                startForYou(gen);
            });
            return;
        }
        startForYou(gen);
    }

    void startForYou(uint32_t gen) {
        mode_ = Mode::ForYou;
        personalBusy_ = true;
        if (g_app->account.signedIn() && !likedLoaded_) {
            async<Page>([] { return g_app->account.likedVideos(""); }, [this, gen](Page& p) {
                likedLoaded_ = true;  // also on failure: the local likes still count
                accountLiked_ = p.items;
                if (gen == feeds_[0].gen) startForYou(gen);
            });
            return;
        }
        if (!hasSeeds()) {
            // Nothing to go on yet: the mixed feed of subscriptions and topics.
            mode_ = Mode::Mix;
            personalBusy_ = false;
            rebuildAll();
            return;
        }
        loadForYouPage(gen);
    }

    // One page: the related videos of the next few seeds, blended.
    void loadForYouPage(uint32_t gen) {
        constexpr size_t kPer = 6;
        Feed& f = feeds_[0];
        std::vector<Item> liked = likedForSeeds();
        size_t total = 0;
        for_you_seeds(g_app->store.history, liked, 0, kPer, &total);
        size_t seedPages = (total + kPer - 1) / kPer;
        std::vector<Item> seeds;
        if ((size_t)fyPage_ < seedPages) {
            seeds = for_you_seeds(g_app->store.history, liked, (size_t)(fyPage_ + fyRot_), kPer);
            seeds.erase(std::remove_if(seeds.begin(), seeds.end(), [&](const Item& it) { return fySeeds_.count(it.id); }),
                        seeds.end());
        }
        if (seeds.empty()) {
            // Past the viewer's own history: carry on from videos the feed already showed.
            for (; fySecond_ < f.items.size() && seeds.size() < kPer; fySecond_++)
                if (!fySeeds_.count(f.items[fySecond_].id)) seeds.push_back(f.items[fySecond_]);
        }
        fyPage_++;
        if (seeds.empty()) {
            personalBusy_ = false;
            f.continuation.clear();
            if (f.items.empty()) {  // no recommendations at all: fall back to the mixed feed
                mode_ = Mode::Mix;
                rebuildAll();
            }
            return;
        }
        for (const Item& it : seeds) fySeeds_.insert(it.id);
        personalBusy_ = true;
        auto related = std::make_shared<std::vector<std::vector<Item>>>(seeds.size());
        auto left = std::make_shared<size_t>(seeds.size());
        for (size_t i = 0; i < seeds.size(); i++) {
            std::string id = seeds[i].id;
            async<WatchInfo>([id] { return g_app->yt.next(id); }, [this, gen, related, left, i](WatchInfo& w) {
                (*related)[i] = std::move(w.related);
                if (--*left > 0 || gen != feeds_[0].gen) return;
                finishForYouPage(*related);
            });
        }
    }

    void finishForYouPage(std::vector<std::vector<Item>>& related) {
        Feed& f = feeds_[0];
        personalBusy_ = false;
        ForYouMix m;
        m.related = std::move(related);
        if (fyRot_ > 0)  // a refresh: a different order than last time
            for (auto& v : m.related) v = shuffled(std::move(v));
        m.exclude = for_you_exclusions(g_app->store.history, g_app->store.disliked);
        for (const std::string& id : g_app->store.notInterested) m.exclude.insert(id);
        for (const std::string& id : fySeeds_) m.exclude.insert(id);
        for (const Item& it : f.items) m.exclude.insert(it.id);
        m.subs = subsItems_;
        for (int i = 0; i < kChips; i++)
            if (def(i).kind == K_TOPIC) m.extra.insert(m.extra.end(), feeds_[i].items.begin(), feeds_[i].items.end());
        std::vector<Item> page = blend_for_you(m, 36);
        std::string selId = f.selected() ? f.selected()->id : "";
        f.items.insert(f.items.end(), page.begin(), page.end());
        f.prune();  // channels the user asked not to see
        f.list.count = f.grid.count = (int)f.items.size();
        reselect(f, selId);
        fyEmpty_ = page.empty() ? fyEmpty_ + 1 : 0;
        f.continuation = fyEmpty_ >= 2 ? "" : "for-you";  // keeps infinite scroll going
        if (f.items.empty()) {
            if (fyEmpty_ < 2) {
                loadForYouPage(f.gen);
            } else {
                f.error = "Couldn't load your recommendations. Check your connection and try again.";
            }
        }
    }

    void loadMorePersonal() {
        Feed& f = feeds_[0];
        uint32_t gen = f.gen;
        if (mode_ == Mode::ForYou) {
            loadForYouPage(gen);
            return;
        }
        std::string cont = f.continuation;
        personalBusy_ = true;
        async<HomeFetch>([cont] { return youtube_home(cont); }, [this, gen](HomeFetch& r) {
            Feed& fi = feeds_[0];
            if (gen != fi.gen) return;
            personalBusy_ = false;
            if (!r.page.error.empty()) {
                fi.continuation.clear();
                return;
            }
            fi.append(r.page);
        });
    }

    // The Shorts shelf on "All": popular Shorts from the Shorts search filter.
    void loadShorts() {
        uint32_t gen = feeds_[0].gen;
        SearchFilters sf;
        sf.type = 9;
        std::string params = search_params(sf);
        async<Page>([params] { return g_app->yt.search("#shorts", "", params); }, [this, gen](Page& p) {
            Feed& f = feeds_[0];
            if (gen != f.gen) return;
            f.shelf = p.shorts;
            if (f.shelf.size() > 15) f.shelf.resize(15);
        });
    }

    void loadSubs() {
        loadedSubs_ = subs_key();
        subsItems_.clear();
        uint32_t gen = ++subsGen_;
        subsPending_ = (int)g_app->store.subs.size();
        subsFailed_ = 0;
        rebuildAll();
        fetchSubsRss([this, gen](std::vector<Item>& items, bool ok) {
            if (gen != subsGen_) return;
            subsPending_--;
            if (!ok) subsFailed_++;
            mergeByDate(subsItems_, items, 200);
            rebuildAll();
        });
    }

    void loadChip(int i, bool more = false) {
        Feed& f = feeds_[i];
        std::string cont = more ? f.continuation : "";
        if (!more) f.reset();
        f.loading = true;
        const ChipDef& d = def(i);
        std::string arg = d.arg;
        Kind kind = d.kind;
        uint32_t gen = f.gen;
        async<Page>([arg, kind, cont] {
            if (kind == K_TOPIC) return g_app->yt.topicFeed(arg);
            SearchFilters sf;
            sf.type = 1;  // videos
            return g_app->yt.search(arg, cont, cont.empty() ? search_params(sf) : "");
        }, [this, i, gen, kind](Page& p) {
            Feed& fi = feeds_[i];
            if (gen != fi.gen) return;
            fi.loading = false;
            if (!p.error.empty()) {
                fi.error = p.error;
                return;
            }
            fi.append(p);
            if (kind == K_TOPIC) fi.continuation.clear();
            if (fi.items.empty() && fi.error.empty()) fi.error = "This feed is empty right now.";
            if (kind == K_TOPIC) rebuildAll();
        });
    }

    // "All" mixes the newest uploads from subscriptions with the topic feeds: two from
    // subscriptions, then one from the next topic, and so on.
    void rebuildAll() {
        if (mode_ != Mode::Mix) {
            rebuildRecent();
            return;
        }
        Feed& f = feeds_[0];
        std::string selId = f.selected() ? f.selected()->id : "";
        std::vector<int> topics;
        for (int i = 0; i < kChips; i++)
            if (def(i).kind == K_TOPIC) topics.push_back(i);
        std::vector<Item> out;
        std::set<std::string> seen;
        auto add = [&](const Item& it) {
            if (seen.insert(it.id).second) out.push_back(it);
        };
        size_t si = 0;
        std::vector<size_t> ti(topics.size(), 0);
        size_t nextTopic = 0;
        for (;;) {
            bool any = false;
            for (int k = 0; k < 2 && si < subsItems_.size(); k++, any = true) add(subsItems_[si++]);
            for (size_t tries = 0; tries < topics.size(); tries++) {
                size_t tt = (nextTopic + tries) % topics.size();
                const std::vector<Item>& src = feeds_[topics[tt]].items;
                if (ti[tt] < src.size()) {
                    add(src[ti[tt]++]);
                    nextTopic = (tt + 1) % topics.size();
                    any = true;
                    break;
                }
            }
            if (!any) break;
        }
        f.items = std::move(out);
        f.prune();
        f.list.count = f.grid.count = (int)f.items.size();
        reselect(f, selId);
        rebuildRecent();
    }

    // "Recently uploaded": subscriptions only, newest first.
    void rebuildRecent() {
        Feed& r = feeds_[kRecent];
        std::string rsel = r.selected() ? r.selected()->id : "";
        r.items = subsItems_;
        r.list.count = r.grid.count = (int)r.items.size();
        reselect(r, rsel);
    }

    void refreshAllState() {
        Feed& a = feeds_[0];
        Feed& r = feeds_[kRecent];
        r.loading = subsPending_ > 0 && r.items.empty();
        if (mode_ != Mode::Mix) {
            a.loading = personalBusy_;
            return;
        }
        bool busy = subsPending_ > 0;
        bool failed = subsFailed_ > 0;
        for (int i = 0; i < kChips; i++)
            if (def(i).kind == K_TOPIC) {
                busy = busy || feeds_[i].loading;
                failed = failed || !feeds_[i].error.empty();
            }
        a.loading = busy;
        if (!busy && a.items.empty()) a.error = failed ? "Couldn't load videos. Check your connection and try again." : "";
        else a.error.clear();
    }

    Feed feeds_[kChips];
    int cur_ = 0;
    bool chipsFocus_ = false;
    int chipSel_ = 0;
    std::vector<Item> subsItems_;
    uint32_t subsGen_ = 0;
    int subsPending_ = 0, subsFailed_ = 0;
    std::string loadedSubs_;

    // What "All" shows: the mix of subscriptions and topics (nothing personal to go on), YouTube's
    // own recommendations for the account, or VivoTube's "For you" built from history and likes.
    enum class Mode { Mix, YouTube, ForYou };
    Mode mode_ = Mode::Mix;
    bool personalBusy_ = false;
    int fyPage_ = 0, fyRot_ = 0, fyEmpty_ = 0;
    size_t fySecond_ = 0;
    std::unordered_set<std::string> fySeeds_;  // videos already used as seeds
    std::vector<Item> accountLiked_;
    bool likedLoaded_ = false;
    uint32_t accountGen_ = 0;
};

// ================================================================ Search page

// Recent searches, shown as a dropdown under the search box, with the keyboard opened on top.
class SearchScreen : public Screen {
public:
    explicit SearchScreen(const std::string& initial) : initial_(initial) {
        list_.rowH = 44;
        openKeyboard(initial_);
    }

    bool isSearchPage() const override { return true; }
    std::string searchQuery() const override { return initial_; }

    void update(Input& in) override {
        auto& s = g_app->store.searches;
        if (in.pressed(BTN_TRIANGLE)) {
            in.consume(BTN_TRIANGLE);
            openKeyboard(initial_);
            return;
        }
        list_.count = 1 + (int)std::min<size_t>(s.size(), kMaxRows);
        list_.focused = true;
        int a = list_.update(in, listRect());
        if (a == 0) {
            openKeyboard(initial_);
            return;
        }
        if (a > 0) {
            std::string q = s[a - 1];
            g_app->openResults(q);
            return;
        }
        int sel = list_.sel;
        if (sel > 0 && sel <= (int)s.size() && in.pressed(BTN_RIGHT)) {
            in.consume(BTN_RIGHT);
            openKeyboard(s[sel - 1]);
        }
        if (sel > 0 && sel <= (int)s.size() && in.pressed(BTN_SQUARE)) {
            std::string q = s[sel - 1];
            g_app->showMenu(q, {"Remove from search history", "Clear all search history"}, [q](int choice) {
                auto& ss = g_app->store.searches;
                if (choice == 0) ss.erase(std::remove(ss.begin(), ss.end(), q), ss.end());
                else ss.clear();
                g_app->store.saveSearches();
            });
        }
    }

    void draw() override {
        auto& s = g_app->store.searches;
        int n = 1 + (int)std::min<size_t>(s.size(), kMaxRows);
        list_.count = n;
        Rect r = listRect();
        float h = std::min(r.h, n * list_.rowH);
        g_gfx.roundRect(r.x, r.y - 8, r.w, h + 16, 12, col::menu);
        list_.focused = active();
        list_.draw(Rect{r.x, r.y, r.w, h}, [&](int i, const Rect& row, bool sel) {
            uint32_t bg = sel ? col::menuSel : col::menu;
            if (sel) g_gfx.rect(row.x, row.y, row.w, row.h, bg);
            float ty = row.y + (row.h - g_gfx.lineHeight(0.78f)) / 2;
            if (i == 0) {
                draw_icon(Icon::Search, row.x + 24, row.y + row.h / 2, 20, col::text, bg);
                g_gfx.text(row.x + 52, ty, col::text2, 0.78f, "Search videos, channels and playlists");
                return;
            }
            draw_icon(Icon::History, row.x + 24, row.y + row.h / 2, 20, col::text, bg);
            g_gfx.textBold(row.x + 52, ty, col::text, 0.78f, g_gfx.ellipsize(s[i - 1], row.w - 110, 0.78f));
            draw_icon(Icon::ArrowUpLeft, row.x + row.w - 26, row.y + row.h / 2, 18, sel ? col::text : col::text3, bg);
        });
        if (s.empty()) {
            g_gfx.textCentered(r.x + r.w / 2, r.y + h + 22, col::text3, 0.66f, "Your recent searches will show up here.");
        }
    }

private:
    static constexpr size_t kMaxRows = 30;
    static Rect listRect() { return Rect{256, TOPBAR_H + 10, 446, SCREEN_H - TOPBAR_H - 40}; }

    void openKeyboard(const std::string& initial) {
        g_app->textInput("Search", initial, guarded([](const std::string& q) {
                             std::string t = trim(q);
                             if (!t.empty()) g_app->openResults(t);
                         }),
                         guarded([this] {
                             if (g_app->store.searches.empty() && g_app->top() == this) g_app->pop();
                         }));
    }

    std::string initial_;
    ListView list_;
};

// ================================================================ Search results

class ResultsScreen : public Screen {
public:
    explicit ResultsScreen(const std::string& q) : query_(q) {
        results_.style = FeedStyle::Rows;
        results_.emptyIcon = Icon::Search;
        results_.emptyTitle = "No results found";
        results_.emptyText = "Try different keywords or remove search filters.";
        run();
    }

    std::string searchQuery() const override { return query_; }

    void update(Input& in) override {
        std::vector<std::string> nm = chipNames();
        int n = (int)nm.size();
        int t = chips_hit(in, nm, kChipsX, kChipsY);
        if (t >= 0) in.consumeTap();
        int cur = chipIndex();
        if (in.pressed(BTN_R)) t = cur < kPresets - 1 ? cur + 1 : 0;
        if (in.pressed(BTN_L)) t = cur > 0 && cur < kPresets ? cur - 1 : kPresets - 1;
        if (chipsFocus_) {
            if (in.repeat(BTN_RIGHT) && chipSel_ + 1 < n) {
                in.consume(BTN_RIGHT);
                chipSel_++;
            }
            if (in.repeat(BTN_LEFT) && chipSel_ > 0) {
                in.consume(BTN_LEFT);
                chipSel_--;
            }
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                t = chipSel_;
            }
            if (in.pressed(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                chipsFocus_ = false;
            }
        }
        if (t >= 0) {
            if (t == kPresets) {
                filtersMenu(0);
            } else if (!(filters_ == preset(t))) {
                filters_ = preset(t);
                run();
            } else {
                results_.scrollToTop();
            }
        }

        results_.setFocused(!chipsFocus_);
        int a = results_.update(in, list());
        if (a >= 0) {
            const Item& it = results_.items[a];
            if (results_.style == FeedStyle::Shorts) g_app->openShort(it, results_.items, a);
            else g_app->openItem(it);
        }
        int sa = results_.takeShelfActivated();
        if (sa >= 0) g_app->openShort(results_.shelf[sa], results_.shelf, sa);
        if (!chipsFocus_ && in.pressed(BTN_UP)) {
            in.consume(BTN_UP);
            chipsFocus_ = true;
            chipSel_ = std::min(chipIndex(), n - 1);
        }
        if (!chipsFocus_ && in.pressed(BTN_SQUARE) && results_.selected()) g_app->itemMenu(*results_.selected());
        if (results_.wantsMore(list())) more();
        if (results_.takeRetry() || results_.takeRefresh()) run();
    }

    void draw() override {
        results_.setFocused(active() && !chipsFocus_);
        results_.draw(list());
        g_gfx.rect(CONTENT.x, CONTENT.y, CONTENT.w, 46, col::bg);
        std::vector<std::string> nm = chipNames();
        draw_chips(nm, chipIndex(), active() && chipsFocus_ ? chipSel_ : -1, kChipsX, kChipsY);
        // The Filters chip carries a filter icon (drawn over its label's left padding).
        float x = kChipsX;
        for (int i = 0; i < kPresets; i++) x += chip_width(nm[i]) + 12;
        bool on = chipIndex() == kPresets;
        draw_icon(Icon::Filter, x + 14, kChipsY + CHIP_H / 2, 16, on ? col::onLight : col::text,
                  on ? col::text : col::raised);
    }

private:
    static constexpr int kPresets = 7;  // chips before "Filters"
    static Rect list() { return Rect{CONTENT.x, CONTENT.y + 46, CONTENT.w, CONTENT.h - 46}; }
    static std::vector<std::string> chipNames() {
        return {"All", "Shorts", "Videos", "Channels", "Playlists", "Recently uploaded", "Live", "     Filters"};
    }
    static SearchFilters preset(int i) {
        SearchFilters f;
        switch (i) {
            case 1: f.type = 9; break;
            case 2: f.type = 1; break;
            case 3: f.type = 2; break;
            case 4: f.type = 3; break;
            case 5: f.upload = 3; break;
            case 6: f.live = true; break;
            default: break;
        }
        return f;
    }
    // The chip matching the current filters; "Filters" when they were set in the dialog.
    int chipIndex() const {
        for (int i = 0; i < kPresets; i++)
            if (filters_ == preset(i)) return i;
        return kPresets;
    }

    void run() {
        results_.reset();
        results_.style = filters_.type == 9 ? FeedStyle::Shorts : FeedStyle::Rows;
        results_.loading = true;
        std::string q = query_, params = search_params(filters_);
        uint32_t gen = results_.gen;
        async<Page>([q, params] { return g_app->yt.search(q, "", params); }, [this, gen](Page& p) {
            if (gen != results_.gen) return;
            results_.loading = false;
            results_.error = p.error;
            results_.append(p);
        });
    }

    void more() {
        results_.loading = true;
        std::string q = query_, cont = results_.continuation;
        uint32_t gen = results_.gen;
        async<Page>([q, cont] { return g_app->yt.search(q, cont); }, [this, gen](Page& p) {
            if (gen != results_.gen) return;
            results_.loading = false;
            if (!p.error.empty()) {
                results_.error = p.error;
                return;
            }
            results_.append(p);
        });
    }

    // ---- Filters dialog: each group opens a list; picking applies it right away.
    struct Choice {
        const char* label;
        int value;
    };
    static const std::vector<Choice>& uploadChoices() {
        static const std::vector<Choice> v = {{"Any time", 0},  {"Last hour", 1},  {"Today", 2},
                                              {"This week", 3}, {"This month", 4}, {"This year", 5}};
        return v;
    }
    static const std::vector<Choice>& typeChoices() {
        static const std::vector<Choice> v = {{"Any type", 0}, {"Videos", 1},    {"Shorts", 9},
                                              {"Channels", 2}, {"Playlists", 3}};
        return v;
    }
    static const std::vector<Choice>& durationChoices() {
        static const std::vector<Choice> v = {
            {"Any duration", 0}, {"Under 3 minutes", 4}, {"3 - 20 minutes", 5}, {"Over 20 minutes", 2}};
        return v;
    }
    static const std::vector<Choice>& sortChoices() {
        static const std::vector<Choice> v = {{"Relevance", 0}, {"Popularity", 3}, {"Upload date", 2}, {"Rating", 1}};
        return v;
    }
    static std::string nameOf(const std::vector<Choice>& v, int value) {
        for (auto& c : v)
            if (c.value == value) return c.label;
        return v.front().label;
    }
    std::string featuresText() const {
        std::vector<std::string> f;
        if (filters_.live) f.push_back("Live");
        if (filters_.uhd) f.push_back("4K");
        if (filters_.hd) f.push_back("HD");
        if (filters_.cc) f.push_back("Subtitles/CC");
        if (f.empty()) return "None";
        std::string s;
        for (auto& x : f) s += (s.empty() ? "" : ", ") + x;
        return s;
    }

    void filtersMenu(int sel) {
        std::vector<std::string> items = {"Upload date: " + nameOf(uploadChoices(), filters_.upload),
                                          "Type: " + nameOf(typeChoices(), filters_.type),
                                          "Duration: " + nameOf(durationChoices(), filters_.duration),
                                          "Features: " + featuresText(),
                                          "Sort by: " + nameOf(sortChoices(), filters_.sort),
                                          "Clear all filters"};
        g_app->showMenu("Search filters", items, guarded([this](int i) {
            switch (i) {
                case 0: pick("Upload date", uploadChoices(), &filters_.upload); break;
                case 1: pick("Type", typeChoices(), &filters_.type); break;
                case 2: pick("Duration", durationChoices(), &filters_.duration); break;
                case 3: features(); break;
                case 4: pick("Sort by", sortChoices(), &filters_.sort); break;
                default:
                    filters_ = SearchFilters();
                    run();
                    break;
            }
        }), sel, -1,
                        {Icon::History, Icon::Playlist, Icon::WatchLater, Icon::Filter, Icon::Sort, Icon::Close});
    }

    void pick(const std::string& title, const std::vector<Choice>& choices, int* field) {
        std::vector<std::string> labels;
        int cur = 0;
        for (size_t i = 0; i < choices.size(); i++) {
            labels.push_back(choices[i].label);
            if (choices[i].value == *field) cur = (int)i;
        }
        const std::vector<Choice>* cs = &choices;
        g_app->showMenu(title, labels, guarded([this, cs, field](int i) {
            *field = (*cs)[i].value;
            run();
        }), cur, cur);
    }

    void features() {
        std::vector<int> checks = {filters_.live, filters_.uhd, filters_.hd, filters_.cc};
        std::weak_ptr<bool> alive = alive_;
        g_app->showChecklist("Features", {"Live", "4K", "HD", "Subtitles/CC"}, checks, [this, alive](int i) -> int {
            if (!alive.lock()) return -2;
            bool* f[4] = {&filters_.live, &filters_.uhd, &filters_.hd, &filters_.cc};
            *f[i] = !*f[i];
            run();
            return *f[i] ? 1 : 0;
        });
    }

    std::string query_;
    SearchFilters filters_;
    Feed results_;
    bool chipsFocus_ = false;
    int chipSel_ = 0;
};

// ================================================================ Subscriptions

class SubscriptionsScreen : public LibraryScreen {
public:
    SubscriptionsScreen() {
        feed_.style = FeedStyle::Grid;
        feed_.grid.padTop = 4;
        feed_.emptyIcon = Icon::Subscriptions;
        feed_.emptyTitle = "No new videos";
        feed_.emptyText = "Your channels haven't posted anything recently.";
        load();
    }

    void onShow() override {
        if (subs_key() != loadedSubs_) load();
    }

    bool scrollToTop() override {
        if (feed_.atVeryTop() && feed_.grid.scroll() <= 1) return false;
        stripFocus_ = false;
        feed_.scrollToTop();
        return true;
    }
    void refresh() override {
        stripFocus_ = false;
        load();
    }

    void update(Input& in) override {
        auto& subs = g_app->store.subs;
        if (subs.empty()) {
            if (in.pressed(BTN_CONFIRM) || (emptyBtn_.w > 0 && in.tapIn(emptyBtn_.x, emptyBtn_.y, emptyBtn_.w, emptyBtn_.h))) {
                in.consume(BTN_CONFIRM);
                in.consumeTap();
                g_app->openSearch();
            }
            return;
        }
        int n = stripCount();
        feed_.grid.headerH = kStripH;
        if (stripFocus_) {
            if (in.repeat(BTN_RIGHT) && stripSel_ + 1 < n) {
                in.consume(BTN_RIGHT);
                stripSel_++;
            }
            if (in.repeat(BTN_LEFT) && stripSel_ > 0) {
                in.consume(BTN_LEFT);
                stripSel_--;
            }
            if (in.pressed(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                stripFocus_ = false;
            }
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                activateStrip(stripSel_);
                return;
            }
            if (in.pressed(BTN_SQUARE) && stripSel_ < (int)subs.size()) {
                const Subscription& s = subs[stripSel_];
                g_app->itemMenu(channelItem(s));
            }
        }
        // Taps on the strip.
        float hx, hy;
        if (feed_.grid.headerTapped(in, grid(), hx, hy)) {
            int i = (int)((hx - 16 + stripScroll_) / kStripStep);
            if (hx > grid().w - 80) i = n - 1;
            if (i >= 0 && i < n) {
                in.consumeTap();
                stripSel_ = i;
                activateStrip(i);
                return;
            }
        }
        feed_.setFocused(!stripFocus_);
        int a = feed_.update(in, grid());
        if (a >= 0) g_app->openItem(feed_.items[a]);
        if (!stripFocus_ && in.pressed(BTN_UP)) {
            in.consume(BTN_UP);
            stripFocus_ = true;
            feed_.grid.scrollToTop();
        }
        if (!stripFocus_ && in.pressed(BTN_SQUARE) && feed_.selected()) g_app->itemMenu(*feed_.selected());
        if (feed_.takeRetry() || feed_.takeRefresh()) load();
    }

    void draw() override {
        auto& subs = g_app->store.subs;
        if (subs.empty()) {
            emptyBtn_ = draw_empty_state(CONTENT, Icon::Subscriptions, "Don't miss new videos",
                                         "Subscribe to channels and their newest uploads will show up here. "
                                         "Subscriptions are saved on this system.",
                                         "Search for channels", active());
            return;
        }
        feed_.grid.headerH = kStripH;
        feed_.setFocused(active() && !stripFocus_);
        feed_.draw(grid(), [&](float y) { drawStrip(y); });
    }

private:
    static constexpr float kStripH = 112, kStripStep = 84;
    static Rect grid() { return CONTENT; }
    int stripCount() const { return (int)g_app->store.subs.size() + 1; }  // channels + "All"

    static Item channelItem(const Subscription& s) {
        Item it;
        it.type = ItemType::Channel;
        it.id = s.id;
        it.title = s.name;
        it.thumb = s.avatar;
        return it;
    }

    void activateStrip(int i) {
        auto& subs = g_app->store.subs;
        if (i < (int)subs.size()) g_app->openChannel(subs[i].id, subs[i].name);
        else g_app->push(make_channels_screen());
    }

    void drawStrip(float y) {
        auto& subs = g_app->store.subs;
        Rect r = grid();
        int n = stripCount();
        bool foc = active() && stripFocus_;
        // "All" sits at the right edge; the avatars scroll beneath it.
        float allW = 72, avail = r.w - 32 - allW;
        float selX = stripSel_ * kStripStep;
        if (stripSel_ < (int)subs.size()) {
            if (selX - stripScroll_ > avail - kStripStep) stripScroll_ = selX - avail + kStripStep;
            if (selX < stripScroll_) stripScroll_ = selX;
        }
        g_gfx.clip(Rect{r.x, std::max(r.y, y), avail + 16, kStripH});
        for (int i = 0; i < (int)subs.size(); i++) {
            float cx = r.x + 16 + i * kStripStep - stripScroll_ + kStripStep / 2;
            if (cx < r.x - kStripStep || cx > r.x + avail + kStripStep) continue;
            draw_avatar(subs[i].avatar, subs[i].name, cx, y + 40, 28);
            if (foc && i == stripSel_) g_gfx.ring(cx, y + 40, 33, 3, col::focus);
            g_gfx.textCentered(cx, y + 76, col::text2, 0.56f, g_gfx.ellipsize(subs[i].name, kStripStep - 8, 0.56f));
        }
        g_gfx.unclip();
        g_gfx.clip(CONTENT);
        float ax = r.x + r.w - 16 - allW;
        bool allFoc = foc && stripSel_ == n - 1;
        if (allFoc) g_gfx.roundRect(ax, y + 26, allW, 30, 15, col::raisedHi);
        g_gfx.textCentered(ax + allW / 2, y + 26 + (30 - g_gfx.lineHeight(0.74f)) / 2, col::blue, 0.74f, "All", true);
        if (allFoc) g_gfx.roundFrame(ax - 3, y + 23, allW + 6, 36, 18, 2.5f, col::focus);
        g_gfx.rect(r.x + 16, y + kStripH - 8, r.w - 32, 1, col::divider);
    }

    void load() {
        loadedSubs_ = subs_key();
        feed_.reset();
        stripSel_ = std::min(stripSel_, stripCount() - 1);
        if (g_app->store.subs.empty()) return;
        feed_.loading = true;
        uint32_t gen = feed_.gen;
        pending_ = (int)g_app->store.subs.size();
        failed_ = 0;
        fetchSubsRss([this, gen](std::vector<Item>& items, bool ok) {
            if (gen != feed_.gen) return;
            if (--pending_ <= 0) feed_.loading = false;
            if (!ok) failed_++;
            std::string selId = feed_.selected() ? feed_.selected()->id : "";
            mergeByDate(feed_.items, items, 250);
            feed_.list.count = feed_.grid.count = (int)feed_.items.size();
            reselect(feed_, selId);
            if (!feed_.loading && feed_.items.empty() && failed_)
                feed_.error = "Couldn't load your subscriptions. Check your connection and try again.";
        });
    }

    Feed feed_;
    bool stripFocus_ = false;
    int stripSel_ = 0;
    float stripScroll_ = 0;
    int pending_ = 0, failed_ = 0;
    std::string loadedSubs_;
    Rect emptyBtn_;
};

// ================================================================ All subscriptions (manage)

class ChannelsScreen : public Screen {
public:
    ChannelsScreen() {
        feed_.style = FeedStyle::Rows;
        feed_.rowH = 120;
        feed_.emptyIcon = Icon::Subscriptions;
        feed_.emptyTitle = "No subscriptions";
        feed_.emptyText = "Channels you subscribe to will be listed here.";
        rebuild();
    }
    void onShow() override { rebuild(); }

    void update(Input& in) override {
        feed_.setFocused(true);
        int a = feed_.update(in, list());
        if (a >= 0) g_app->openChannel(feed_.items[a].id, feed_.items[a].title);
        if (in.pressed(BTN_SQUARE) && feed_.selected()) {
            Item it = *feed_.selected();
            g_app->showMenu(it.title, {"Go to channel", "Unsubscribe"}, guarded([this, it](int i) {
                if (i == 0) g_app->openChannel(it.id, it.title);
                else {
                    g_app->toggleSubscription(it.id, it.title, it.thumb);
                    rebuild();
                }
            }));
        }
    }

    void draw() override {
        page_title(CONTENT.x + 24, CONTENT.y + 10, "All subscriptions");
        g_gfx.text(CONTENT.x + 25, CONTENT.y + 38, col::text2, 0.66f,
                   plural((int)feed_.items.size(), "channel", "channels") + " " G_DOT " A-Z");
        feed_.setFocused(active());
        feed_.draw(list());
    }

private:
    static Rect list() { return Rect{CONTENT.x, CONTENT.y + 64, CONTENT.w, CONTENT.h - 64}; }
    void rebuild() {
        int sel = feed_.sel();
        feed_.items.clear();
        for (const Subscription& s : g_app->store.subs) {
            Item it;
            it.type = ItemType::Channel;
            it.id = s.id;
            it.title = s.name;
            it.thumb = s.avatar;
            feed_.items.push_back(it);
        }
        std::sort(feed_.items.begin(), feed_.items.end(), [](const Item& a, const Item& b) {
            std::string x = a.title, y = b.title;
            std::transform(x.begin(), x.end(), x.begin(), ::tolower);
            std::transform(y.begin(), y.end(), y.begin(), ::tolower);
            return x < y;
        });
        feed_.setSel(sel);
    }
    Feed feed_;
};

// ================================================================ You (library)

// Cards for the library's playlists: Watch later, Liked videos and playlists made on this system.
static std::vector<Item> library_playlists() {
    std::vector<Item> v;
    auto card = [&](const std::string& id, const std::string& title, const std::vector<Item>& items) {
        Item pl;
        pl.type = ItemType::Playlist;
        pl.id = id;
        pl.title = title;
        pl.thumb = items.empty() ? "" : items.front().thumbUrl();
        pl.extra = plural((int)items.size(), "video", "videos");
        v.push_back(pl);
    };
    card("WL", "Watch later", g_app->store.watchLater);
    card("LL", "Liked videos", g_app->store.liked);
    for (const LocalPlaylist& p : g_app->store.playlists) card(p.id, p.title, p.items);
    return v;
}

static void open_library_playlist(const Item& pl) {
    if (pl.id == "WL") g_app->push(make_watch_later_screen());
    else if (pl.id == "LL") g_app->openLiked();
    else g_app->openLocalPlaylist(pl.id);
}

static Icon library_icon(const std::string& id) {
    return id == "WL" ? Icon::WatchLater : id == "LL" ? Icon::ThumbUp : Icon::Playlist;
}

class YouScreen : public Screen {
public:
    YouScreen() { rebuild(); }
    void onShow() override { rebuild(); }

    void update(Input& in) override {
        if (accountGen_ != g_app->accountGen) rebuild();
        int n = (int)rows_.size();
        if (in.repeat(BTN_DOWN) && row_ + 1 < n) {
            in.consume(BTN_DOWN);
            row_++;
        }
        if (in.repeat(BTN_UP) && row_ > 0) {
            in.consume(BTN_UP);
            row_--;
        }
        Row& r = rows_[row_];
        if (r.kind != Row::Link) {
            int cnt = shelfCount(r);
            if (in.repeat(BTN_RIGHT) && r.sel + 1 < cnt) {
                in.consume(BTN_RIGHT);
                r.sel++;
            }
            if (in.repeat(BTN_LEFT) && r.sel > 0) {
                in.consume(BTN_LEFT);
                r.sel--;
            }
        }
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            activate(row_, r.sel);
            return;
        }
        if (in.pressed(BTN_SQUARE) && r.kind == Row::History && r.sel < (int)history_.size())
            g_app->itemMenu(history_[r.sel]);

        // Touch: vertical drag scrolls the page; taps activate.
        if (in.touching && in.dragging && CONTENT.contains(in.startX, in.startY)) {
            scroll_ -= in.dy;
            target_ = -1;
        }
        if (in.tapped && CONTENT.contains(in.tapX, in.tapY)) {
            int hr, hi;
            if (hitTest(in.tapX, in.tapY, hr, hi)) {
                in.consumeTap();
                row_ = hr;
                if (hi >= 0) rows_[hr].sel = hi;
                activate(hr, hi >= 0 ? hi : 0);
                return;
            }
        }
        layout();
        if (!(in.touching && in.dragging)) {
            if (target_ >= 0) scroll_ += (target_ - scroll_) * 0.3f;
            scroll_ = std::max(0.0f, std::min(scroll_, std::max(0.0f, totalH_ - CONTENT.h)));
        }
    }

    void draw() override {
        layout();
        g_gfx.clip(CONTENT);
        float x = CONTENT.x + 24, y0 = CONTENT.y - scroll_;
        if (g_app->account.signedIn()) {
            GoogleProfile p = g_app->account.profile();
            draw_avatar(p.avatar, p.name, x + 36, y0 + 52, 36);
            g_gfx.text(x + 92, y0 + 22, col::text, 1.3f, g_gfx.ellipsize(p.name, CONTENT.w - 140, 1.3f), W_BOLD);
            std::string line = p.handle.empty() ? "Signed in with Google" : p.handle + G_DOT "View channel";
            g_gfx.text(x + 93, y0 + 58, col::text2, 0.66f, line);
        } else {
            g_gfx.circle(x + 36, y0 + 52, 36, col::raised);
            draw_icon(Icon::YouFilled, x + 36, y0 + 52, 40, col::text2, col::raised);
            g_gfx.text(x + 92, y0 + 24, col::text, 1.3f, "You", W_BOLD);
            g_gfx.text(x + 93, y0 + 58, col::text2, 0.66f,
                       "Your library is saved on this system " G_DOT " " +
                           plural((int)g_app->store.subs.size(), "subscription", "subscriptions"));
        }
        bool act = active();
        for (int i = 0; i < (int)rows_.size(); i++) {
            const Row& r = rows_[i];
            float y = y0 + r.y;
            bool foc = act && i == row_;
            if (r.kind == Row::Link) {
                drawLink(r, x - 8, y, CONTENT.w - 32, foc);
                continue;
            }
            g_gfx.text(x, y, col::text, 1.0f, r.kind == Row::History ? "History" : "Playlists", W_BOLD);
            drawShelf(r, x, y + kTitleH, foc);
        }
        g_gfx.unclip();
    }

private:
    struct Row {
        enum Kind { History, Playlists, Link } kind;
        int link = 0;
        int sel = 0;
        float y = 0, h = 0, scrollX = 0;
    };
    static constexpr float kHeaderH = 112, kTitleH = 36, kCardW = 200, kCardGap = 14, kLinkH = 48;
    static constexpr int kLinks = 6;
    static float cardThumbH() { return std::floor(kCardW * 9.0f / 16.0f); }
    static float shelfH() { return cardThumbH() + 52; }

    enum LinkId { L_HISTORY, L_WATCH_LATER, L_LIKED, L_SUBS, L_ACCOUNT, L_SETTINGS };
    static Icon linkIcon(int i) {
        static const Icon icons[kLinks] = {Icon::History, Icon::WatchLater, Icon::ThumbUp,
                                           Icon::Subscriptions, Icon::Account, Icon::Settings};
        return icons[i];
    }
    static std::string linkLabel(int i) {
        switch (i) {
            case L_HISTORY: return "History";
            case L_WATCH_LATER: return "Watch later";
            case L_LIKED: return "Liked videos";
            case L_SUBS: return "Your subscriptions";
            case L_ACCOUNT: return g_app->account.signedIn() ? "Your Google account" : "Sign in with Google";
            default: return "Settings";
        }
    }

    void rebuild() {
        accountGen_ = g_app->accountGen;
        history_.clear();
        progress_.clear();
        for (const HistoryEntry& h : g_app->store.history) {
            if (history_.size() >= 12) break;
            history_.push_back(h.item);
            progress_.push_back(g_app->store.watchedFraction(h.item.id));
        }
        playlists_ = library_playlists();
        int oldRow = row_;
        std::vector<int> oldSel;
        for (auto& r : rows_) oldSel.push_back(r.sel);
        rows_.clear();
        rows_.push_back(Row{Row::History});
        rows_.push_back(Row{Row::Playlists});
        for (int i = 0; i < kLinks; i++) {
            Row r{Row::Link};
            r.link = i;
            rows_.push_back(r);
        }
        for (size_t i = 0; i < rows_.size() && i < oldSel.size(); i++)
            rows_[i].sel = std::max(0, std::min(oldSel[i], shelfCount(rows_[i]) - 1));
        row_ = std::min(oldRow, (int)rows_.size() - 1);
    }

    int shelfCount(const Row& r) const {
        if (r.kind == Row::History) return (int)history_.size() + 1;  // + "View all"
        if (r.kind == Row::Playlists) return (int)playlists_.size();
        return 1;
    }

    void layout() {
        float y = kHeaderH;
        for (auto& r : rows_) {
            r.y = y;
            r.h = r.kind == Row::Link ? kLinkH : kTitleH + shelfH() + 16;
            y += r.h;
            if (r.kind == Row::Playlists) y += 8;
        }
        totalH_ = y + 16;
        // Keep the focused row on screen.
        const Row& f = rows_[row_];
        float top = row_ == 0 ? 0 : f.y - 8, bottom = f.y + f.h + 8;
        float t = target_ >= 0 ? target_ : scroll_;
        if (top < t) t = top;
        if (bottom > t + CONTENT.h) t = bottom - CONTENT.h;
        target_ = std::max(0.0f, std::min(t, std::max(0.0f, totalH_ - CONTENT.h)));
        // Horizontal shelf scrolling.
        for (auto& r : rows_) {
            if (r.kind == Row::Link) continue;
            float avail = CONTENT.w - 48;
            float sx = r.sel * (kCardW + kCardGap);
            if (sx - r.scrollX > avail - kCardW) r.scrollX = sx - avail + kCardW;
            if (sx < r.scrollX) r.scrollX = sx;
        }
    }

    void drawShelf(const Row& r, float x, float y, bool foc) {
        float th = cardThumbH();
        if (r.kind == Row::History) {
            for (int i = 0; i <= (int)history_.size(); i++) {
                float cx = x + i * (kCardW + kCardGap) - r.scrollX;
                if (cx > CONTENT.x + CONTENT.w || cx + kCardW < CONTENT.x) continue;
                bool f = foc && r.sel == i;
                if (i == (int)history_.size()) {
                    drawViewAll(cx, y, th, f);
                    continue;
                }
                const Item& it = history_[i];
                draw_thumb(it, cx, y, kCardW, th, 10, col::bg, progress_[i]);
                if (f) draw_focus_ring(cx, y, kCardW, th, 10);
                float ty = y + th + 8;
                ty += g_gfx.paragraph(cx, ty, kCardW - 4, col::text, 0.66f, it.title, 2, 1.0f, W_MEDIUM);
                draw_channel_name(cx, ty + 1, col::text2, 0.58f, it.channel, it.verified, kCardW - 4);
            }
            if (history_.empty())
                g_gfx.text(x + kCardW + 30, y + th / 2 - 10, col::text2, 0.7f, "Videos you watch will show up here.");
            return;
        }
        for (int i = 0; i < (int)playlists_.size(); i++) {
            const Item& pl = playlists_[i];
            float cx = x + i * (kCardW + kCardGap) - r.scrollX;
            if (cx > CONTENT.x + CONTENT.w || cx + kCardW < CONTENT.x) continue;
            bool f = foc && r.sel == i;
            draw_thumb(pl, cx, y, kCardW, th, 10);
            if (pl.thumb.empty()) draw_icon(library_icon(pl.id), cx + kCardW / 2, y + th / 2, 36, col::text3, col::thumbBg);
            if (f) draw_focus_ring(cx, y, kCardW, th, 10);
            g_gfx.text(cx, y + th + 8, col::text, 0.66f, g_gfx.ellipsize(pl.title, kCardW - 4, 0.66f, W_MEDIUM), W_MEDIUM);
            g_gfx.text(cx, y + th + 26, col::text2, 0.58f, "Private" G_DOT + pl.extra);
        }
    }

    void drawViewAll(float x, float y, float th, bool foc) {
        float cx = x + kCardW / 2 - 40, cy = y + th / 2;
        g_gfx.circle(cx, cy, 28, foc ? col::raisedHi : col::raised);
        draw_icon(Icon::ChevronRight, cx, cy, 24, col::text, foc ? col::raisedHi : col::raised);
        if (foc) g_gfx.ring(cx, cy, 33, 3, col::focus);
        g_gfx.textCentered(cx, cy + 38, col::text, 0.66f, "View all", W_MEDIUM);
    }

    void drawLink(const Row& r, float x, float y, float w, bool foc) {
        uint32_t bg = foc ? col::rowHi : col::bg;
        if (foc) g_gfx.roundRect(x, y + 2, w, kLinkH - 4, 10, bg);
        draw_icon(linkIcon(r.link), x + 28, y + kLinkH / 2, 22, col::text, bg);
        g_gfx.text(x + 60, y + (kLinkH - g_gfx.lineHeight(0.8f)) / 2, col::text, 0.8f, linkLabel(r.link));
        std::string count;
        if (r.link == L_WATCH_LATER) count = std::to_string(g_app->store.watchLater.size());
        if (r.link == L_LIKED) count = std::to_string(g_app->store.liked.size());
        if (r.link == L_SUBS) count = std::to_string(g_app->store.subs.size());
        if (!count.empty()) g_gfx.textRight(x + w - 20, y + (kLinkH - g_gfx.lineHeight(0.7f)) / 2, col::text2, 0.7f, count);
    }

    bool hitTest(float px, float py, int& row, int& item) {
        layout();
        float y = py - CONTENT.y + scroll_;
        float x0 = CONTENT.x + 24;
        for (int i = 0; i < (int)rows_.size(); i++) {
            const Row& r = rows_[i];
            if (y < r.y || y >= r.y + r.h) continue;
            row = i;
            item = -1;
            if (r.kind == Row::Link) return true;
            float sy = r.y + kTitleH;
            if (y < sy || y > sy + shelfH()) return false;
            int idx = (int)((px - x0 + r.scrollX) / (kCardW + kCardGap));
            if (idx < 0 || idx >= shelfCount(r)) return false;
            item = idx;
            return true;
        }
        return false;
    }

    void activate(int row, int sel) {
        const Row& r = rows_[row];
        if (r.kind == Row::History) {
            if (sel < (int)history_.size()) g_app->openVideo(history_[sel]);
            else g_app->push(make_history_screen());
        } else if (r.kind == Row::Playlists) {
            if (sel < (int)playlists_.size()) open_library_playlist(playlists_[sel]);
        } else {
            switch (r.link) {
                case L_HISTORY: g_app->push(make_history_screen()); break;
                case L_WATCH_LATER: g_app->push(make_watch_later_screen()); break;
                case L_LIKED: g_app->openLiked(); break;
                case L_SUBS: g_app->push(make_channels_screen()); break;
                case L_ACCOUNT: g_app->accountMenu(); break;
                default: g_app->openSettings(); break;
            }
        }
    }

    std::vector<Item> history_, playlists_;
    std::vector<float> progress_;
    std::vector<Row> rows_;
    int row_ = 0;
    float scroll_ = 0, target_ = -1, totalH_ = 0;
    uint32_t accountGen_ = 0;
};

// ================================================================ History

class HistoryScreen : public Screen {
public:
    HistoryScreen() {
        feed_.style = FeedStyle::Rows;
        feed_.rowH = 140;
        feed_.emptyIcon = Icon::History;
        feed_.emptyTitle = "No watch history yet";
        feed_.emptyText = "Videos you watch will show up here. History is kept on this system only.";
        rebuild();
    }
    void onShow() override { rebuild(); }

    void update(Input& in) override {
        feed_.setFocused(true);
        int a = feed_.update(in, list());
        if (a >= 0) g_app->openVideo(feed_.items[a]);
        if (in.pressed(BTN_SQUARE)) {
            std::vector<std::pair<std::string, std::function<void()>>> extra;
            const Item* s = feed_.selected();
            if (s) {
                std::string id = s->id;
                extra.push_back({"Remove from watch history", guarded([this, id] {
                                     g_app->store.removeHistory(id);
                                     g_app->store.saveHistory();
                                     rebuild();
                                 })});
            }
            extra.push_back({"Clear all watch history", guarded([this] {
                                 g_app->confirm("Clear all watch history?", "Clear watch history", guarded([this] {
                                     g_app->store.clearHistory();
                                     g_app->store.saveHistory();
                                     rebuild();
                                     g_app->toast("Watch history cleared");
                                 }));
                             })});
            if (s) g_app->itemMenu(*s, extra);
            else g_app->showMenu("Watch history", {extra.back().first}, [extra](int) { extra.back().second(); });
        }
    }

    void draw() override {
        page_title(CONTENT.x + 24, CONTENT.y + 10, "Watch history");
        g_gfx.text(CONTENT.x + 25, CONTENT.y + 38, col::text2, 0.66f,
                   plural((int)feed_.items.size(), "video", "videos") + " " G_DOT " kept on this system");
        feed_.setFocused(active());
        feed_.draw(list());
    }

private:
    static Rect list() { return Rect{CONTENT.x, CONTENT.y + 64, CONTENT.w, CONTENT.h - 64}; }
    void rebuild() {
        int sel = feed_.sel();
        feed_.items.clear();
        feed_.progress.clear();
        for (const HistoryEntry& h : g_app->store.history) {
            feed_.items.push_back(h.item);
            feed_.progress.push_back(g_app->store.watchedFraction(h.item.id));
        }
        feed_.setSel(sel);
    }
    Feed feed_;
};

// ================================================================ Playlist-style pages

// Left: an info card with cover, title, counts and Play all / Shuffle. Right: numbered rows.
class PlaylistLayout : public Screen {
public:
    PlaylistLayout() {
        feed_.style = FeedStyle::Compact;
        feed_.rowH = 88;
    }

    void update(Input& in) override {
        bool canPlay = !feed_.items.empty();
        if (btnFocus_ >= 0) {
            if (in.repeat(BTN_RIGHT)) {
                in.consume(BTN_RIGHT);
                if (btnFocus_ == 0) btnFocus_ = 1;
                else if (canPlay) btnFocus_ = -1;
            }
            if (in.repeat(BTN_LEFT) && btnFocus_ == 1) {
                in.consume(BTN_LEFT);
                btnFocus_ = 0;
            }
            if (in.pressed(BTN_DOWN) && canPlay) {
                in.consume(BTN_DOWN);
                btnFocus_ = -1;
            }
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                if (btnFocus_ == 0) playAll();
                else playShuffled();
                return;
            }
        }
        if (in.tapIn(playBtn_.x, playBtn_.y, playBtn_.w, playBtn_.h)) {
            in.consumeTap();
            playAll();
            return;
        }
        if (in.tapIn(shuffleBtn_.x, shuffleBtn_.y, shuffleBtn_.w, shuffleBtn_.h)) {
            in.consumeTap();
            playShuffled();
            return;
        }
        feed_.setFocused(btnFocus_ < 0);
        int a = feed_.update(in, list());
        if (a >= 0) g_app->openVideo(feed_.items[a], feed_.items, a);
        if (btnFocus_ < 0 && in.pressed(BTN_LEFT)) {
            in.consume(BTN_LEFT);
            btnFocus_ = 0;
        }
        if (btnFocus_ < 0 && in.pressed(BTN_SQUARE) && feed_.selected()) itemOptions(*feed_.selected());
        else if (btnFocus_ >= 0 && in.pressed(BTN_SQUARE)) pageOptions();
        if (feed_.takeRetry() || feed_.takeRefresh()) reload();
        if (feed_.wantsMore(list())) loadMore();
    }

    void draw() override {
        float x = CONTENT.x + 12, y = CONTENT.y + 6, w = kPanelW, h = CONTENT.h - 18;
        // Panel tinted from the top like a cover-colored card.
        g_gfx.vgradient(x, y, w, h, rgba(58, 52, 64), rgba(24, 24, 26));
        g_gfx.cornerMask(x, y, w, h, 16, col::bg);
        float cx = x + 14, cw = w - 28, ch = std::floor(cw * 9.0f / 16.0f);
        float cy = y + 14;
        Item cover;
        cover.type = ItemType::Video;
        if (!feed_.items.empty()) cover = feed_.items.front();
        cover.duration.clear();
        cover.live = false;
        draw_thumb(cover, cx, cy, cw, ch, 10, rgba(55, 50, 61), -1, false);
        if (feed_.items.empty()) draw_icon(coverIcon(), cx + cw / 2, cy + ch / 2, 40, col::text3, col::thumbBg);
        float ty = cy + ch + 12;
        ty += g_gfx.paragraph(cx, ty, cw, col::text, 1.0f, pageTitle(), 2, 1.0f, true) + 4;
        std::string by = byline();
        if (!by.empty()) {
            g_gfx.textBold(cx, ty, col::text, 0.68f, g_gfx.ellipsize(by, cw, 0.68f));
            ty += g_gfx.lineHeight(0.68f) + 2;
        }
        g_gfx.text(cx, ty, col::text2, 0.62f, g_gfx.ellipsize(stats(), cw, 0.62f));
        ty += g_gfx.lineHeight(0.62f) + 12;
        bool act = active();
        float pw = (cw - 10) / 2;
        playBtn_ = Rect{cx, ty, pw, 36};
        shuffleBtn_ = Rect{cx + pw + 10, ty, pw, 36};
        drawWideButton(playBtn_, "Play all", Icon::Play, true, act && btnFocus_ == 0);
        drawWideButton(shuffleBtn_, "Shuffle", Icon::Shuffle, false, act && btnFocus_ == 1);
        std::string desc = description();
        if (!desc.empty()) g_gfx.paragraph(cx, ty + 50, cw, col::text2, 0.6f, desc, 4);

        feed_.setFocused(act && btnFocus_ < 0);
        Rect r = list();
        if (feed_.items.empty()) {
            feed_.draw(r);
            return;
        }
        feed_.list.focused = act && btnFocus_ < 0;
        feed_.list.count = (int)feed_.items.size();
        feed_.list.draw(r, [&](int i, const Rect& row, bool sel) { draw_compact_row(feed_.items[i], row, sel, i); });
        if (feed_.loading) g_gfx.spinner(r.x + r.w / 2, r.y + r.h - 20, 10, col::text2);
    }

protected:
    static constexpr float kPanelW = 300;
    static Rect list() {
        float x = CONTENT.x + kPanelW + 24;
        return Rect{x, CONTENT.y + 4, SCREEN_W - x - 4, CONTENT.h - 4};
    }
    virtual std::string pageTitle() const = 0;
    virtual std::string byline() const { return ""; }
    virtual std::string stats() const { return plural((int)feed_.items.size(), "video", "videos"); }
    virtual std::string description() const { return ""; }
    virtual Icon coverIcon() const { return Icon::Playlist; }
    virtual void itemOptions(const Item& it) { g_app->itemMenu(it); }
    virtual void pageOptions() {}
    virtual void reload() {}
    virtual void loadMore() {}

    void playAll() {
        if (!feed_.items.empty()) g_app->openVideo(feed_.items[0], feed_.items, 0);
    }
    void playShuffled() {
        if (feed_.items.empty()) return;
        std::vector<Item> q = shuffled(feed_.items);
        g_app->openVideo(q[0], q, 0);
    }

    static void drawWideButton(const Rect& r, const char* label, Icon icon, bool primary, bool foc) {
        uint32_t fill = primary ? col::text : foc ? col::raisedHi : rgba(255, 255, 255, 40);
        uint32_t fg = primary ? col::onLight : col::text;
        g_gfx.roundRect(r.x, r.y, r.w, r.h, r.h / 2, fill);
        float tw = (float)g_gfx.width(label, 0.72f, true) + 22;
        float sx = r.x + (r.w - tw) / 2;
        draw_icon(icon, sx + 8, r.y + r.h / 2, 16, fg, fill);
        g_gfx.textBold(sx + 22, r.y + (r.h - g_gfx.lineHeight(0.72f)) / 2, fg, 0.72f, label);
        if (foc) g_gfx.roundFrame(r.x - 4, r.y - 4, r.w + 8, r.h + 8, r.h / 2 + 4, 2.5f, col::focus);
    }

    Feed feed_;
    int btnFocus_ = -1;  // -1: the list; 0: Play all; 1: Shuffle
    Rect playBtn_, shuffleBtn_;
};

class WatchLaterScreen : public PlaylistLayout {
public:
    WatchLaterScreen() {
        feed_.emptyIcon = Icon::WatchLater;
        feed_.emptyTitle = "No videos saved";
        feed_.emptyText = "Press " G_SQUARE " on any video and choose Save to Watch later.";
        rebuild();
    }
    void onShow() override { rebuild(); }

protected:
    std::string pageTitle() const override { return "Watch later"; }
    std::string byline() const override { return "On this system"; }
    std::string stats() const override {
        return "Private " G_DOT " " + plural((int)feed_.items.size(), "video", "videos");
    }
    Icon coverIcon() const override { return Icon::WatchLater; }
    void itemOptions(const Item& it) override {
        std::string id = it.id;
        g_app->itemMenu(it, {{"Remove from Watch later", guarded([this, id] {
                                  g_app->store.removeWatchLater(id);
                                  g_app->store.saveWatchLater();
                                  rebuild();
                              })}});
    }

private:
    void rebuild() {
        int sel = feed_.sel();
        feed_.items = g_app->store.watchLater;
        feed_.setSel(sel);
        if (feed_.items.empty()) btnFocus_ = -1;
    }
};

class PlaylistScreen : public PlaylistLayout {
public:
    PlaylistScreen(const std::string& id, const std::string& title) : id_(id), title_(title) {
        feed_.emptyIcon = Icon::Playlist;
        feed_.emptyTitle = "This playlist is empty";
        load(false);
    }

protected:
    std::string pageTitle() const override { return title_.empty() ? "Playlist" : title_; }
    std::string byline() const override { return author_; }
    std::string stats() const override {
        std::string s = "Playlist";
        if (!count_.empty()) s += G_DOT + count_;
        return s;
    }
    std::string description() const override { return desc_; }
    void reload() override { load(false); }
    void loadMore() override { load(true); }

private:
    void load(bool more) {
        std::string cont = more ? feed_.continuation : "";
        if (!more) feed_.reset();
        feed_.loading = true;
        std::string id = id_;
        uint32_t gen = feed_.gen;
        async<PlaylistPage>([id, cont] { return g_app->yt.playlist(id, cont); }, [this, gen](PlaylistPage& p) {
            if (gen != feed_.gen) return;
            feed_.loading = false;
            if (!p.title.empty()) title_ = p.title;
            if (!p.author.empty()) author_ = p.author;
            if (!p.count.empty()) count_ = p.count;
            if (!p.description.empty()) desc_ = p.description;
            if (!p.error.empty()) {
                feed_.error = p.error;
                return;
            }
            feed_.append(p);
        });
    }

    std::string id_, title_, author_, count_, desc_;
};

// Liked videos: the account's liked list when signed in, otherwise the ones liked on this system.
class LikedScreen : public PlaylistLayout {
public:
    LikedScreen() {
        feed_.emptyIcon = Icon::ThumbUp;
        feed_.emptyTitle = "No liked videos yet";
        feed_.emptyText = "Tap the thumbs up on a video to save it here.";
        account_ = g_app->account.signedIn();
        if (account_) load(false);
        else rebuild();
    }
    void onShow() override {
        if (!account_) rebuild();
    }

protected:
    std::string pageTitle() const override { return "Liked videos"; }
    std::string byline() const override {
        return account_ ? g_app->account.profile().name : std::string("On this system");
    }
    std::string stats() const override { return "Private" G_DOT + plural((int)feed_.items.size(), "video", "videos"); }
    Icon coverIcon() const override { return Icon::ThumbUp; }
    void reload() override {
        if (account_) load(false);
    }
    void loadMore() override {
        if (account_) load(true);
    }
    void itemOptions(const Item& it) override {
        Item copy = it;
        g_app->itemMenu(it, {{"Remove from Liked videos", guarded([this, copy] {
                                  g_app->rateVideo(copy, 0);
                                  auto& v = feed_.items;
                                  for (size_t i = 0; i < v.size(); i++)
                                      if (v[i].id == copy.id) {
                                          v.erase(v.begin() + i);
                                          break;
                                      }
                                  feed_.list.count = (int)v.size();
                              })}});
    }

private:
    void rebuild() {
        int sel = feed_.sel();
        feed_.items = g_app->store.liked;
        feed_.setSel(sel);
        if (feed_.items.empty()) btnFocus_ = -1;
    }
    void load(bool more) {
        std::string cont = more ? feed_.continuation : "";
        if (!more) feed_.reset();
        feed_.loading = true;
        uint32_t gen = feed_.gen;
        async<Page>([cont] { return g_app->account.likedVideos(cont); }, [this, gen](Page& p) {
            if (gen != feed_.gen) return;
            feed_.loading = false;
            if (!p.error.empty()) {
                feed_.error = p.error;
                return;
            }
            feed_.append(p);
        });
    }
    bool account_ = false;
};

// A playlist kept on this system.
class LocalPlaylistScreen : public PlaylistLayout {
public:
    explicit LocalPlaylistScreen(const std::string& id) : id_(id) {
        feed_.emptyIcon = Icon::Playlist;
        feed_.emptyTitle = "No videos in this playlist yet";
        feed_.emptyText = "Press " G_SQUARE " on a video and choose Save to playlist.";
        rebuild();
    }
    void onShow() override { rebuild(); }

protected:
    std::string pageTitle() const override { return title_; }
    std::string byline() const override { return "On this system"; }
    std::string stats() const override {
        return "Private" G_DOT + plural((int)feed_.items.size(), "video", "videos") + G_DOT G_SQUARE " Options";
    }
    void itemOptions(const Item& it) override {
        Item copy = it;
        std::string id = id_, title = title_;
        g_app->itemMenu(it, {{"Remove from " + title, guarded([this, copy, id] {
                                  if (g_app->store.inPlaylist(id, copy.id)) g_app->store.togglePlaylistItem(id, copy);
                                  g_app->store.savePlaylists();
                                  rebuild();
                              })}});
    }
    void pageOptions() override {
        std::string id = id_;
        g_app->showActions(title_, {{"Rename playlist", Icon::Plus, guarded([this, id] {
                                         g_app->textInput("Playlist name", title_, guarded([this, id](const std::string& t) {
                                             std::string n = trim(t);
                                             LocalPlaylist* p = g_app->store.playlist(id);
                                             if (n.empty() || !p) return;
                                             p->title = n;
                                             g_app->store.savePlaylists();
                                             rebuild();
                                         }));
                                     })},
                                    {"Delete playlist", Icon::Trash, guarded([this, id] {
                                         g_app->confirm("Delete " + title_ + "?", "Delete", guarded([this, id] {
                                             g_app->store.deletePlaylist(id);
                                             g_app->store.savePlaylists();
                                             g_app->toast("Playlist deleted");
                                             if (g_app->top() == this) g_app->pop();
                                         }));
                                     })}});
    }

private:
    void rebuild() {
        LocalPlaylist* p = g_app->store.playlist(id_);
        int sel = feed_.sel();
        feed_.items = p ? p->items : std::vector<Item>();
        if (p) title_ = p->title;
        feed_.setSel(sel);
        if (feed_.items.empty()) btnFocus_ = -1;
    }
    std::string id_, title_ = "Playlist";
};

// One of the signed-in account's playlists (read through the YouTube Data API, so private
// playlists work too).
class AccountPlaylistScreen : public PlaylistLayout {
public:
    AccountPlaylistScreen(const std::string& id, const std::string& title) : id_(id), title_(title) {
        feed_.emptyIcon = Icon::Playlist;
        feed_.emptyTitle = "This playlist is empty";
        load(false);
    }

protected:
    std::string pageTitle() const override { return title_; }
    std::string byline() const override { return g_app->account.profile().name; }
    void reload() override { load(false); }
    void loadMore() override { load(true); }

private:
    void load(bool more) {
        std::string cont = more ? feed_.continuation : "", id = id_;
        if (!more) feed_.reset();
        feed_.loading = true;
        uint32_t gen = feed_.gen;
        async<Page>([id, cont] { return g_app->account.playlistItems(id, cont); }, [this, gen](Page& p) {
            if (gen != feed_.gen) return;
            feed_.loading = false;
            if (!p.error.empty()) {
                feed_.error = p.error;
                return;
            }
            feed_.append(p);
        });
    }
    std::string id_, title_;
};

// Grid of playlists with a page title; subclasses provide the items and what opening one does.
class PlaylistGridScreen : public Screen {
public:
    PlaylistGridScreen() {
        feed_.style = FeedStyle::Grid;
        feed_.showAvatars = false;
        feed_.grid.padTop = 6;
        feed_.emptyIcon = Icon::Playlist;
    }

    void update(Input& in) override {
        if (btnFocus_) {
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                button();
                return;
            }
            if (in.pressed(BTN_DOWN) && !feed_.items.empty()) {
                in.consume(BTN_DOWN);
                btnFocus_ = false;
            }
        }
        if (hasButton() && in.tapIn(btn_.x - 4, btn_.y - 4, btn_.w + 8, btn_.h + 8)) {
            in.consumeTap();
            button();
            return;
        }
        feed_.setFocused(!btnFocus_);
        int a = feed_.update(in, grid());
        if (a >= 0) open(feed_.items[a]);
        if (!btnFocus_ && hasButton() && in.pressed(BTN_UP)) {
            in.consume(BTN_UP);
            btnFocus_ = true;
        }
        if (!btnFocus_ && in.pressed(BTN_SQUARE) && feed_.selected()) options(*feed_.selected());
        if (feed_.takeRetry() || feed_.takeRefresh()) reload();
    }

    void draw() override {
        page_title(CONTENT.x + 24, CONTENT.y + 10, title());
        if (hasButton()) {
            float w = pill_width(buttonLabel(), true);
            btn_ = Rect{CONTENT.x + CONTENT.w - 24 - w, CONTENT.y + 8, w, 36};
            draw_pill(btn_.x, btn_.y, buttonLabel(), PillStyle::Secondary, active() && btnFocus_, true, Icon::Plus);
        }
        feed_.setFocused(active() && !btnFocus_);
        feed_.draw(grid());
    }

protected:
    static Rect grid() { return Rect{CONTENT.x, CONTENT.y + 52, CONTENT.w, CONTENT.h - 52}; }
    virtual std::string title() const = 0;
    virtual void open(const Item& it) = 0;
    virtual void options(const Item& it) { (void)it; }
    virtual void reload() {}
    virtual bool hasButton() const { return false; }
    virtual std::string buttonLabel() const { return ""; }
    virtual void button() {}
    Feed feed_;
    bool btnFocus_ = false;
    Rect btn_;
};

// "Playlists": Watch later, Liked videos and the playlists made on this system.
class PlaylistsScreen : public PlaylistGridScreen {
public:
    PlaylistsScreen() {
        feed_.emptyTitle = "No playlists yet";
        rebuild();
    }
    void onShow() override { rebuild(); }

protected:
    std::string title() const override { return "Playlists"; }
    void open(const Item& it) override { open_library_playlist(it); }
    bool hasButton() const override { return true; }
    std::string buttonLabel() const override { return "New playlist"; }
    void button() override {
        g_app->textInput("New playlist", "", guarded([this](const std::string& t) {
            std::string n = trim(t);
            if (n.empty()) return;
            g_app->store.createPlaylist(n);
            g_app->store.savePlaylists();
            g_app->toast("Created " + n);
            rebuild();
        }));
    }
    void options(const Item& it) override {
        if (it.id == "WL" || it.id == "LL") return;
        std::string id = it.id, title = it.title;
        g_app->showActions(title, {{"Open", Icon::Playlist, [id] { g_app->openLocalPlaylist(id); }},
                                   {"Delete playlist", Icon::Trash, guarded([this, id, title] {
                                        g_app->confirm("Delete " + title + "?", "Delete", guarded([this, id] {
                                            g_app->store.deletePlaylist(id);
                                            g_app->store.savePlaylists();
                                            rebuild();
                                        }));
                                    })}});
    }

private:
    void rebuild() {
        int sel = feed_.sel();
        feed_.items = library_playlists();
        feed_.setSel(sel);
    }
};

// The signed-in account's playlists on YouTube.
class AccountPlaylistsScreen : public PlaylistGridScreen {
public:
    AccountPlaylistsScreen() {
        feed_.emptyTitle = "No playlists on your account";
        reload();
    }

protected:
    std::string title() const override { return "Your YouTube playlists"; }
    void open(const Item& it) override { g_app->push(make_account_playlist_screen(it.id, it.title)); }
    void reload() override {
        feed_.reset();
        feed_.loading = true;
        uint32_t gen = feed_.gen;
        struct R {
            std::vector<Item> items;
            std::string err;
            bool ok = false;
        };
        async<R>([] {
            R r;
            r.ok = g_app->account.myPlaylists(r.items, &r.err);
            return r;
        }, [this, gen](R& r) {
            if (gen != feed_.gen) return;
            feed_.loading = false;
            if (!r.ok) {
                feed_.error = r.err;
                return;
            }
            feed_.items = r.items;
        });
    }
};

// ================================================================ Settings

class SettingsScreen : public Screen {
public:
    SettingsScreen() { build(); }

    void update(Input& in) override {
        syncList();
        int a = list_.update(in, listRect());
        if (a >= 0) activate(rows_[a]);
    }

    void draw() override {
        syncList();
        list_.focused = active();
        list_.draw(listRect(), [&](int i, const Rect& rc, bool sel) {
            const Row& r = rows_[i];
            float y = rc.y;
            if (!r.section.empty()) {
                if (i > 0) g_gfx.rect(rc.x + 16, y + 6, rc.w - 40, 1, col::divider);
                g_gfx.textBold(rc.x + 24, y + 16, col::text, 0.86f, r.section);
                y += kSectionH;
            }
            uint32_t bg = sel ? col::rowHi : col::bg;
            if (sel) g_gfx.roundRect(rc.x + 8, y + 2, rc.w - 24, kRowH - 4, 10, bg);
            std::string sub = r.sub ? r.sub() : "";
            float ly = sub.empty() ? y + (kRowH - g_gfx.lineHeight(0.8f)) / 2 : y + 11;
            g_gfx.text(rc.x + 24, ly, col::text, 0.8f, r.label);
            if (!sub.empty()) g_gfx.text(rc.x + 24, ly + 22, col::text2, 0.64f, g_gfx.ellipsize(sub, rc.w - 140, 0.64f));
            if (r.kind == Row::Toggle) draw_switch(rc.x + rc.w - 76, y + (kRowH - 24) / 2, r.get());
            if (r.kind == Row::Choice || r.kind == Row::Action)
                draw_icon(Icon::ChevronRight, rc.x + rc.w - 48, y + kRowH / 2, 18, col::text2, bg);
        });
    }

private:
    struct Row {
        enum Kind { Toggle, Choice, Action, Info } kind;
        std::string section, label;
        std::function<std::string()> sub;
        std::function<bool()> get;                 // Toggle
        std::function<void()> act;                 // Toggle / Action / Choice
    };
    static constexpr float kRowH = 60, kSectionH = 46;
    static Rect listRect() { return Rect{CONTENT.x + 40, CONTENT.y + 2, CONTENT.w - 80, CONTENT.h - 2}; }

    void syncList() {
        if ((int)list_.heights.size() != (int)rows_.size()) {
            list_.heights.clear();
            for (auto& r : rows_) list_.heights.push_back(kRowH + (r.section.empty() ? 0 : kSectionH));
        }
        list_.count = (int)rows_.size();
    }

    void activate(Row& r) {
        if (r.act) r.act();
    }

    static void choose(const std::string& title, const std::vector<std::string>& labels, int cur,
                       std::function<void(int)> set) {
        g_app->showMenu(title, labels, set, std::max(0, cur), cur);
    }

    template <class T>
    static int indexOf(const std::vector<T>& v, const T& x) {
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == x) return (int)i;
        return -1;
    }

    void build() {
        Settings* st = &g_app->store.settings;
        Store* store = &g_app->store;
        static const std::vector<int> heights = {144, 240, 360, 480, 720};
        // YouTube's language codes (it still uses "iw" for Hebrew), each shown in English and in
        // its own script.
        static const std::vector<std::string> langs = {
            "en", "es", "fr", "de", "it", "pt", "nl", "pl", "sv", "tr", "vi", "id", "ms", "fil",
            "el", "ru", "uk", "ka", "hy", "ar", "iw", "fa", "ur", "hi", "mr", "bn", "pa", "gu",
            "ta", "te", "kn", "ml", "si", "th", "lo", "km", "my", "am", "ja", "ko", "zh-Hans", "zh-Hant"};
        static const std::vector<std::string> langNames = {
            "English", "Spanish" G_DOT "Español", "French" G_DOT "Français", "German" G_DOT "Deutsch",
            "Italian" G_DOT "Italiano", "Portuguese" G_DOT "Português", "Dutch" G_DOT "Nederlands",
            "Polish" G_DOT "Polski", "Swedish" G_DOT "Svenska", "Turkish" G_DOT "Türkçe",
            "Vietnamese" G_DOT "Tiếng Việt", "Indonesian" G_DOT "Bahasa Indonesia", "Malay" G_DOT "Bahasa Melayu",
            "Filipino", "Greek" G_DOT "Ελληνικά", "Russian" G_DOT "Русский", "Ukrainian" G_DOT "Українська",
            "Georgian" G_DOT "ქართული", "Armenian" G_DOT "Հայերեն", "Arabic" G_DOT "العربية",
            "Hebrew" G_DOT "עברית", "Persian" G_DOT "فارسی", "Urdu" G_DOT "اردو", "Hindi" G_DOT "हिन्दी",
            "Marathi" G_DOT "मराठी", "Bengali" G_DOT "বাংলা", "Punjabi" G_DOT "ਪੰਜਾਬੀ",
            "Gujarati" G_DOT "ગુજરાતી", "Tamil" G_DOT "தமிழ்", "Telugu" G_DOT "తెలుగు", "Kannada" G_DOT "ಕನ್ನಡ",
            "Malayalam" G_DOT "മലയാളം", "Sinhala" G_DOT "සිංහල", "Thai" G_DOT "ไทย", "Lao" G_DOT "ລາວ",
            "Khmer" G_DOT "ខ្មែរ", "Burmese" G_DOT "မြန်မာ", "Amharic" G_DOT "አማርኛ", "Japanese" G_DOT "日本語",
            "Korean" G_DOT "한국어", "Chinese, Simplified" G_DOT "简体中文", "Chinese, Traditional" G_DOT "繁體中文"};
        static const std::vector<std::string> regions = {
            "US", "GB", "CA", "AU", "IE", "NZ", "IN", "PK", "BD", "LK", "NP", "DE", "FR", "ES", "IT", "NL",
            "SE", "PL", "GR", "TR", "RU", "UA", "GE", "AM", "IL", "SA", "AE", "EG", "MA", "NG", "KE", "ZA",
            "BR", "MX", "AR", "CO", "JP", "KR", "TW", "HK", "TH", "VN", "ID", "MY", "PH", "SG"};
        static const std::vector<std::string> regionNames = {
            "United States", "United Kingdom", "Canada", "Australia", "Ireland", "New Zealand", "India", "Pakistan",
            "Bangladesh", "Sri Lanka", "Nepal", "Germany", "France", "Spain", "Italy", "Netherlands", "Sweden",
            "Poland", "Greece", "Türkiye", "Russia", "Ukraine", "Georgia", "Armenia", "Israel", "Saudi Arabia",
            "United Arab Emirates", "Egypt", "Morocco", "Nigeria", "Kenya", "South Africa", "Brazil", "Mexico",
            "Argentina", "Colombia", "Japan", "South Korea", "Taiwan", "Hong Kong", "Thailand", "Vietnam",
            "Indonesia", "Malaysia", "Philippines", "Singapore"};
        auto langName = [](const std::string& code) {
            int i = indexOf(langs, code);
            return i >= 0 ? langNames[i] : code;
        };
        auto regionName = [](const std::string& code) {
            int i = indexOf(regions, code);
            return i >= 0 ? regionNames[i] : code;
        };

        // Playback
        rows_.push_back({Row::Choice, "Playback", "Preferred video quality",
                         [st] { return str_printf("%dp " G_DOT " used when available", st->maxHeight); }, nullptr,
                         [st, store] {
                             std::vector<std::string> l;
                             for (int h : heights) l.push_back(str_printf("%dp", h));
                             choose("Preferred video quality", l, indexOf(heights, st->maxHeight), [st, store](int i) {
                                 st->maxHeight = heights[i];
                                 store->saveSettings();
                             });
                         }});
        rows_.push_back({Row::Toggle, "", "Autoplay",
                         [] { return std::string("Play the next video in playlists and Watch later"); },
                         [st] { return st->autoplay; },
                         [st, store] {
                             st->autoplay = !st->autoplay;
                             store->saveSettings();
                         }});
        rows_.push_back({Row::Toggle, "", "Resume where you left off",
                         [] { return std::string("Start videos from your last watched position"); },
                         [st] { return st->rememberPosition; },
                         [st, store] {
                             st->rememberPosition = !st->rememberPosition;
                             store->saveSettings();
                         }});
        // Captions
        rows_.push_back({Row::Toggle, "Captions", "Always show captions",
                         [] { return std::string("When a video has them in your language"); },
                         [st] { return st->captions; },
                         [st, store] {
                             st->captions = !st->captions;
                             store->saveSettings();
                         }});
        rows_.push_back({Row::Choice, "", "Caption language", [st, langName] { return langName(st->captionLang); },
                         nullptr, [st, store] {
                             choose("Caption language", langNames, indexOf(langs, st->captionLang), [st, store](int i) {
                                 st->captionLang = langs[i];
                                 store->saveSettings();
                             });
                         }});
        // Language & region
        rows_.push_back({Row::Choice, "Language & region", "Content language", [st, langName] { return langName(st->hl); },
                         nullptr, [st, store] {
                             choose("Content language", langNames, indexOf(langs, st->hl), [st, store](int i) {
                                 st->hl = langs[i];
                                 store->saveSettings();
                                 g_app->applyLocale();
                             });
                         }});
        rows_.push_back({Row::Choice, "", "Location", [st, regionName] { return regionName(st->gl); }, nullptr, [st, store] {
                             choose("Location", regionNames, indexOf(regions, st->gl), [st, store](int i) {
                                 st->gl = regions[i];
                                 store->saveSettings();
                                 g_app->applyLocale();
                             });
                         }});
        // History & privacy
        rows_.push_back({Row::Action, "History & privacy", "Clear watch history",
                         [] { return std::string("Also clears resume positions"); }, nullptr, [store] {
                             g_app->confirm("Clear watch history?", "Clear watch history", [store] {
                                 store->clearHistory();
                                 store->saveHistory();
                                 g_app->toast("Watch history cleared");
                             });
                         }});
        rows_.push_back({Row::Action, "", "Clear search history", nullptr, nullptr, [store] {
                             g_app->confirm("Clear search history?", "Clear search history", [store] {
                                 store->clearSearches();
                                 store->saveSearches();
                                 g_app->toast("Search history cleared");
                             });
                         }});
        rows_.push_back({Row::Action, "", "Hidden recommendations",
                         [store] {
                             size_t v = store->notInterested.size(), c = store->blockedChannels.size();
                             if (!v && !c) return std::string("Nothing hidden");
                             return str_printf("%u video%s, %u channel%s", (unsigned)v, v == 1 ? "" : "s",
                                               (unsigned)c, c == 1 ? "" : "s");
                         },
                         nullptr, [store] {
                             if (store->notInterested.empty() && store->blockedChannels.empty()) {
                                 g_app->toast("Nothing is hidden from your recommendations");
                                 return;
                             }
                             std::vector<std::string> opts{"Show everything again"};
                             for (const Subscription& s : store->blockedChannels)
                                 opts.push_back("Recommend " + (s.name.empty() ? s.id : s.name) + " again");
                             g_app->showMenu("Hidden recommendations", opts, [store](int i) {
                                 if (i == 0) {
                                     store->clearHidden();
                                     g_app->toast("Hidden videos and channels can be recommended again");
                                 } else if (i > 0 && i <= (int)store->blockedChannels.size()) {
                                     std::string name = store->blockedChannels[i - 1].name;
                                     store->unblockChannel((size_t)i - 1);
                                     g_app->toast(name.empty() ? std::string("Channel can be recommended again")
                                                               : name + " can be recommended again");
                                 }
                             });
                         }});
        rows_.push_back({Row::Action, "", "Remove all subscriptions", nullptr, nullptr, [store] {
                             g_app->confirm("Remove all subscriptions?", "Remove all", [store] {
                                 store->subs.clear();
                                 store->saveSubs();
                                 g_app->toast("Subscriptions removed");
                             });
                         }});
        // Account
        rows_.push_back({Row::Action, "Account", "Google account",
                         [] {
                             if (g_app->account.signedIn()) return "Signed in as " + g_app->account.profile().name;
                             return std::string(g_app->account.configured()
                                                    ? "Sign in to sync likes, subscriptions and playlists"
                                                    : "Not set up " G_DOT " see the README to add your OAuth client");
                         },
                         nullptr, [] { g_app->accountMenu(); }});
        rows_.push_back({Row::Action, "", "Sync subscriptions now",
                         [] { return std::string("Adds your account's subscriptions to this system"); }, nullptr, [] {
                             if (g_app->account.signedIn()) g_app->syncSubscriptions(false);
                             else g_app->toast("Sign in first");
                         }});
        // Updates
        rows_.push_back({Row::Toggle, "Updates", "Check for updates when VivoTube starts",
                         [] { return std::string("New versions are published on GitHub"); },
                         [st] { return st->checkUpdates; },
                         [st, store] {
                             st->checkUpdates = !st->checkUpdates;
                             store->saveSettings();
                         }});
        rows_.push_back({Row::Action, "", "Check for updates now", [] { return std::string("Current version " VT_VERSION); },
                         nullptr, [] { g_app->checkForUpdates(true); }});
        // About
        rows_.push_back({Row::Action, "About", "Controls", [] { return std::string("Buttons and touch"); }, nullptr, [] {
                             std::string c = g_app->confirmGlyph(), b = g_app->cancelGlyph();
                             g_app->showMenu("Controls",
                                             {c + "  Open / play / pause", b + "  Back", G_TRIANGLE "  Search",
                                              G_SQUARE "  More options for the highlighted video",
                                              "L / R  Switch chips and tabs, seek 10 s while watching",
                                              "SELECT  Guide (menu); fullscreen while watching",
                                              "START  Play / pause the video or the mini player",
                                              b + " while watching  Keep playing in the mini player",
                                              "Mini player  Down from the last tab; " + c + " expand, " G_SQUARE " close",
                                              "Tab again  Scroll to the top, then refresh",
                                              "Pull down a feed  Refresh"},
                                             [](int) {});
                         }});
        rows_.push_back({Row::Info, "", "VivoTube " VT_VERSION,
                         [] { return std::string("Unofficial client " G_DOT " not affiliated with YouTube or Google"); },
                         nullptr, nullptr});
        rows_.push_back({Row::Info, "", "Data folder",
                         [] { return g_app->dataDir + "  (clients.json here overrides the built-in client profiles)"; },
                         nullptr, nullptr});
    }

    std::vector<Row> rows_;
    ListView list_;
};

// ================================================================ Channel

class ChannelScreen : public Screen {
public:
    ChannelScreen(const std::string& id, const std::string& name) : id_(id), name_(name) {
        for (Feed& f : feeds_) {
            f.style = FeedStyle::Grid;
            f.showAvatars = false;
            f.grid.headerH = kHeaderH;
        }
        feeds_[0].emptyIcon = Icon::Playlist;
        feeds_[0].emptyTitle = "This channel doesn't have any videos";
        feeds_[1].emptyIcon = Icon::Playlist;
        feeds_[1].emptyTitle = "This channel has no playlists";
        load(0);
    }

    void update(Input& in) override {
        Feed& f = feeds_[cur_];
        int t = -1;
        if (in.pressed(BTN_R)) t = 1;
        if (in.pressed(BTN_L)) t = 0;
        float hx, hy;
        if (f.grid.headerTapped(in, grid(), hx, hy)) {
            in.consumeTap();
            if (subBtn_.contains(in.tapX, in.tapY)) toggleSub();
            for (int i = 0; i < 2; i++)
                if (hy >= kTabsY - 6 && in.tapX >= tabX_[i] - 8 && in.tapX < tabX_[i] + tabW_[i] + 8) t = i;
            (void)hx;
        }
        if (focus_ == F_SUB) {
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                toggleSub();
            }
            if (in.pressed(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                focus_ = F_TABS;
            }
        } else if (focus_ == F_TABS) {
            if (in.repeat(BTN_RIGHT) && cur_ == 0) {
                in.consume(BTN_RIGHT);
                t = 1;
            }
            if (in.repeat(BTN_LEFT) && cur_ == 1) {
                in.consume(BTN_LEFT);
                t = 0;
            }
            if (in.pressed(BTN_UP)) {
                in.consume(BTN_UP);
                focus_ = F_SUB;
            }
            if (in.pressed(BTN_DOWN) || in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_DOWN | BTN_CONFIRM);
                if (!feeds_[cur_].items.empty()) focus_ = F_GRID;
            }
        }
        if (t >= 0 && t != cur_) {
            cur_ = t;
            Feed& nf = feeds_[cur_];
            nf.grid.reset();
            if (nf.items.empty() && !nf.loading && nf.error.empty()) load(cur_);
        }
        Feed& g = feeds_[cur_];
        g.setFocused(focus_ == F_GRID);
        int a = g.update(in, grid());
        if (a >= 0) g_app->openItem(g.items[a]);
        if (focus_ == F_GRID && in.pressed(BTN_UP)) {
            in.consume(BTN_UP);
            focus_ = F_TABS;
            g.grid.scrollToTop();
        }
        if (g.wantsMore(grid())) load(cur_, true);
        if (g.takeRetry() || g.takeRefresh()) load(cur_);
        if (focus_ == F_GRID && in.pressed(BTN_SQUARE) && g.selected()) g_app->itemMenu(*g.selected());
    }

    void draw() override {
        Feed& f = feeds_[cur_];
        f.setFocused(active() && focus_ == F_GRID);
        f.draw(grid(), [&](float y) { drawHeader(y); });
    }

private:
    enum Focus { F_GRID, F_TABS, F_SUB };
    static constexpr float kHeaderH = 196, kTabsY = 150;
    static Rect grid() { return CONTENT; }

    void drawHeader(float y) {
        Rect r = grid();
        float x = r.x + 24;
        bool act = active();
        draw_avatar(avatar_, name_, x + 46, y + 66, 46);
        float tx = x + 116;
        g_gfx.textBold(tx, y + 24, col::text, 1.3f, g_gfx.ellipsize(name_, r.x + r.w - tx - 24, 1.3f));
        std::string meta = subscribers_.empty() ? "Channel" : subscribers_;
        g_gfx.text(tx, y + 60, col::text2, 0.68f, g_gfx.ellipsize(meta, r.x + r.w - tx - 24, 0.68f));
        bool sub = g_app->store.isSubscribed(id_);
        float bw = draw_pill(tx, y + 86, sub ? "Subscribed" : "Subscribe", sub ? PillStyle::Secondary : PillStyle::Primary,
                             act && focus_ == F_SUB);
        subBtn_ = Rect{tx, y + 86, bw, 36};
        // Underline tabs.
        const char* names[2] = {"Videos", "Playlists"};
        float lx = x;
        for (int i = 0; i < 2; i++) {
            float w = (float)g_gfx.width(names[i], 0.8f, true);
            bool on = i == cur_;
            tabX_[i] = lx;
            tabW_[i] = w;
            g_gfx.textBold(lx, y + kTabsY + 4, on ? col::text : col::text2, 0.8f, names[i]);
            if (on) g_gfx.rect(lx, y + kTabsY + 30, w, 3, col::text);
            if (on && act && focus_ == F_TABS) g_gfx.roundFrame(lx - 10, y + kTabsY - 4, w + 20, 40, 8, 2.5f, col::focus);
            lx += w + 34;
        }
        g_gfx.rect(r.x, y + kTabsY + 33, r.w, 1, col::divider);
    }

    void toggleSub() { g_app->toggleSubscription(id_, name_, avatar_); }

    void load(int tab, bool more = false) {
        Feed& f = feeds_[tab];
        std::string cont;
        if (more) cont = f.continuation;
        else f.reset();
        f.loading = true;
        std::string id = id_;
        uint32_t gen = f.gen;
        ChannelTab ct = tab == 0 ? ChannelTab::Videos : ChannelTab::Playlists;
        async<ChannelPage>([id, ct, cont] { return g_app->yt.channel(id, ct, cont); }, [this, tab, gen](ChannelPage& p) {
            Feed& fi = feeds_[tab];
            if (gen != fi.gen) return;
            fi.loading = false;
            if (!p.title.empty()) name_ = p.title;
            if (!p.avatar.empty()) avatar_ = p.avatar;
            if (!p.subscribers.empty()) subscribers_ = p.subscribers;
            // Keep the stored subscription's name and avatar fresh.
            Store& s = g_app->store;
            for (auto& sub : s.subs) {
                if (sub.id == id_ && (sub.name != name_ || (!avatar_.empty() && sub.avatar != avatar_))) {
                    sub.name = name_;
                    if (!avatar_.empty()) sub.avatar = avatar_;
                    s.saveSubs();
                }
            }
            if (!p.error.empty()) {
                fi.error = p.error;
                return;
            }
            for (auto& it : p.items) {
                if (it.channel.empty()) it.channel = name_;
                if (it.channelId.empty()) it.channelId = id_;
                if (it.avatar.empty()) it.avatar = avatar_;
            }
            fi.append(p);
        });
    }

    std::string id_, name_, avatar_, subscribers_;
    Feed feeds_[2];
    int cur_ = 0;
    Focus focus_ = F_GRID;
    Rect subBtn_;
    float tabX_[2] = {0, 0}, tabW_[2] = {0, 0};
};

// ================================================================ factories

std::unique_ptr<Screen> make_home_screen() { return std::make_unique<HomeScreen>(); }
std::unique_ptr<Screen> make_subscriptions_screen() { return std::make_unique<SubscriptionsScreen>(); }
std::unique_ptr<Screen> make_you_screen() { return std::make_unique<YouScreen>(); }
std::unique_ptr<Screen> make_search_screen(const std::string& initial) { return std::make_unique<SearchScreen>(initial); }
std::unique_ptr<Screen> make_results_screen(const std::string& q) { return std::make_unique<ResultsScreen>(q); }
std::unique_ptr<Screen> make_history_screen() { return std::make_unique<HistoryScreen>(); }
std::unique_ptr<Screen> make_watch_later_screen() { return std::make_unique<WatchLaterScreen>(); }
std::unique_ptr<Screen> make_liked_screen() { return std::make_unique<LikedScreen>(); }
std::unique_ptr<Screen> make_local_playlist_screen(const std::string& id) {
    return std::make_unique<LocalPlaylistScreen>(id);
}
std::unique_ptr<Screen> make_account_playlist_screen(const std::string& id, const std::string& title) {
    return std::make_unique<AccountPlaylistScreen>(id, title);
}
std::unique_ptr<Screen> make_account_playlists_screen() { return std::make_unique<AccountPlaylistsScreen>(); }
std::unique_ptr<Screen> make_playlists_screen() { return std::make_unique<PlaylistsScreen>(); }
std::unique_ptr<Screen> make_channels_screen() { return std::make_unique<ChannelsScreen>(); }
std::unique_ptr<Screen> make_settings_screen() { return std::make_unique<SettingsScreen>(); }
std::unique_ptr<Screen> make_channel_screen(const std::string& id, const std::string& name) {
    return std::make_unique<ChannelScreen>(id, name);
}
std::unique_ptr<Screen> make_playlist_screen(const std::string& id, const std::string& title) {
    return std::make_unique<PlaylistScreen>(id, title);
}

}  // namespace vt
