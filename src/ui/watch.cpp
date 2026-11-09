// Watch page: the player with overlay controls, title / channel / actions / description below it,
// and a side column with Up next / Related / Comments / About.
#include <algorithm>
#include <cmath>
#include <set>

#include "../core/captions.h"
#include "../core/parse.h"
#include "../core/util.h"
#include "../vita/player.h"
#include "app.h"
#include "comments.h"

namespace vt {

namespace {

constexpr Rect VIDEO{0, 0, 640, 360};
constexpr Rect FULL{0, 0, 960, 544};
constexpr float PANEL_X = 652;
constexpr float PANEL_W = SCREEN_W - PANEL_X;
constexpr float CHIPS_Y = 8;
constexpr Rect PANEL_LIST{PANEL_X - 6, 50, PANEL_W + 6, SCREEN_H - 50};
constexpr float kChipScale = 0.64f, kChipGap = 8;
constexpr float kPillH = 32;

enum PanelTab { TAB_QUEUE, TAB_RELATED, TAB_COMMENTS, TAB_CHAPTERS, TAB_INFO };

std::string friendly_status(const PlayerInfo& p) {
    std::string r = p.reason.empty() ? p.status : p.reason;
    if (p.status == "LOGIN_REQUIRED")
        return "YouTube asks to sign in to watch this video (age-restricted, private or a bot check). " + r;
    if (p.status.empty()) return "Couldn't load this video. " + r;
    return r;
}

// Playback speed lasts for the session, across videos, like on the web.
double g_speed = 1.0;
const double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};

std::string speed_label(double s) {
    if (s == 1.0) return "Normal";
    std::string t = str_printf("%.2f", s);
    while (t.back() == '0') t.pop_back();
    if (t.back() == '.') t.pop_back();
    return t + "x";
}

}  // namespace

class WatchScreen : public Screen {
public:
    WatchScreen(const Item& it, const std::vector<Item>& queue, int qi) : item_(it), queue_(queue), qi_(qi) {
        if (qi_ < 0 || qi_ >= (int)queue_.size()) {
            queue_.clear();
            qi_ = -1;
        }
        if (!queue_.empty()) tabs_.push_back(TAB_QUEUE);
        tabs_.push_back(TAB_RELATED);
        tabs_.push_back(TAB_COMMENTS);
        tabs_.push_back(TAB_INFO);
        queueFeed_.items = queue_;
        queueFeed_.style = FeedStyle::Compact;
        queueFeed_.rowH = 82;
        queueFeed_.setSel(std::max(0, qi_));
        related_.style = FeedStyle::Compact;
        related_.rowH = 82;
        related_.emptyIcon = Icon::Playlist;
        related_.emptyTitle = "No related videos";
        related_.hide = [](const Item& it) { return g_app->store.hidden(it); };
        related_.loading = true;
        chapterList_.rowH = 56;
        desc_.list.focused = false;
        capsOn_ = g_app->store.settings.captions;
        lastInteract_ = monotonic_seconds();
        load();
    }

    ~WatchScreen() override { stopPlayer(); }

    bool fullscreen() const override { return true; }
    bool isWatch() const override { return true; }
    bool playsVideo() const override { return true; }

    // ------------------------------------------------------------ mini player

    bool canMinimize() const override {
        return playError_.empty() && !(player_ && player_->state() == Player::State::Error);
    }

    void setMinimized(bool) override {
        full_ = false;
        countdown_ = false;
        seekPending_ = false;
        lastInteract_ = monotonic_seconds();
    }

    void tickMinimized() override {
        double now = monotonic_seconds();
        Player::State st = player_ ? player_->state() : Player::State::Loading;
        if (player_ && (st == Player::State::Playing || st == Player::State::Buffering)) g_app->keepAwake();
        tickProgress(now, st);
        if (countdown_) {  // ended with another video queued: the mini player moves on to it
            countdown_ = false;
            playQueue(qi_ + 1, true);
        }
    }

    void togglePlay() override { togglePause(); }
    std::string videoId() const override { return item_.id; }

    // "Play next in queue" / "Add to queue": starts a queue with this video if there is none.
    bool enqueue(const Item& it, bool next) override {
        if (it.id == item_.id) return false;
        if (queue_.empty()) {
            queue_.push_back(item_);
            qi_ = 0;
            tabs_.insert(tabs_.begin(), TAB_QUEUE);
            tab_++;
        }
        for (size_t k = qi_ + 1; k < queue_.size();) {
            if (queue_[k].id == it.id) queue_.erase(queue_.begin() + k);
            else k++;
        }
        size_t at = next ? (size_t)qi_ + 1 : queue_.size();
        queue_.insert(queue_.begin() + at, it);
        int sel = queueFeed_.sel();
        queueFeed_.items = queue_;
        queueFeed_.setSel(std::max(0, std::min(sel, (int)queue_.size() - 1)));
        return true;
    }

    void drawMini(const Rect& r, bool focused) override {
        Rect v{r.x, r.y, r.w, MINI_VIDEO_H};
        g_gfx.roundRect(r.x - 2, r.y - 1, r.w + 4, r.h + 4, 12, rgba(0, 0, 0, 120));  // shadow
        g_gfx.roundRect(r.x, r.y, r.w, r.h, 10, col::menu);
        g_gfx.rect(v.x, v.y + 6, v.w, v.h - 6, rgba(0, 0, 0));
        g_gfx.roundRect(v.x, v.y, v.w, 16, 10, rgba(0, 0, 0));
        Player::State st = player_ ? player_->state() : Player::State::Loading;
        if (player_) {
            player_->draw(v.x, v.y, v.w, v.h);
        } else if (vita2d_texture* t = g_app->thumbs.get(item_.thumbUrl())) {
            g_gfx.imageCover(t, v.x, v.y, v.w, v.h);
        }
        bool busy = st == Player::State::Loading || st == Player::State::Buffering || !infoLoaded_;
        if (busy) g_gfx.spinner(v.x + v.w / 2, v.y + v.h / 2, 16, col::text);
        double dur = shownDur(), pos = player_ ? player_->position() : 0;
        float frac = dur > 0 ? (float)std::min(1.0, pos / dur) : 0;
        g_gfx.rect(v.x, v.y + v.h - 3, v.w, 3, rgba(255, 255, 255, 60));
        g_gfx.rect(v.x, v.y + v.h - 3, v.w * frac, 3, col::red);

        float by = v.y + v.h, tx = r.x + 12, tw = MINI_PLAY_X - 22 - tx;
        std::string title = item_.title.empty() ? "Loading..." : item_.title;
        g_gfx.text(tx, by + 5, col::text, 0.62f, g_gfx.ellipsize(title, tw, 0.62f, W_MEDIUM), W_MEDIUM);
        std::string sub = focused ? g_app->confirmGlyph() + " Expand   " G_SQUARE " Close   START Pause" : item_.channel;
        g_gfx.text(tx, by + 25, col::text2, 0.54f, g_gfx.ellipsize(sub, tw, 0.54f));
        bool playing = player_ && !player_->paused() && st != Player::State::Ended;
        draw_icon(playing ? Icon::Pause : Icon::Play, MINI_PLAY_X, MINI_BTN_Y, 22, col::text, col::menu);
        draw_icon(Icon::Close, MINI_CLOSE_X, MINI_BTN_Y, 20, col::text, col::menu);
        if (focused) g_gfx.roundFrame(r.x - 4, r.y - 4, r.w + 8, r.h + 8, 14, 2.5f, col::focus);
    }

    void onHide() override {
        if (player_) {
            resume_ = player_->position();
            resumeOnShow_ = true;
        }
        stopPlayer();
    }

    void onShow() override {
        if (resumeOnShow_ && haveStreams_) startPlayer(resume_);
        resumeOnShow_ = false;
    }

    // ------------------------------------------------------------ input

    void update(Input& in) override {
        double now = monotonic_seconds();
        // New presses, auto-repeat and touches count as activity (a stuck "held" button must not
        // keep the controls up forever).
        if (in.pressed(0xFFFF) || in.repeat(0xFFFF) || in.touching) lastInteract_ = now;
        Player::State st = player_ ? player_->state() : Player::State::Loading;
        if (player_ && (st == Player::State::Playing || st == Player::State::Buffering)) g_app->keepAwake();

        tickProgress(now, st);
        if (pendingTap_ && now >= pendingTapAt_) {
            pendingTap_ = false;
            singleTap(now);
        }
        if (handleCountdown(in, now)) return;

        if (in.pressed(BTN_SELECT)) toggleFull();
        if (in.pressed(BTN_CANCEL) && full_) {
            in.consume(BTN_CANCEL);
            full_ = false;
            return;
        }
        if (in.pressed(BTN_SQUARE)) {
            in.consume(BTN_SQUARE);
            if (!full_ && focus_ == F_PANEL && panelSelectedItem())
                g_app->itemMenu(*panelSelectedItem(), {}, tabs_[tab_] == TAB_RELATED);
            else optionsMenu();
            return;
        }
        if (in.pressed(BTN_START)) togglePause();
        if (full_ && in.pressed(BTN_TRIANGLE)) in.consume(BTN_TRIANGLE);

        // Seeking with L/R (and the d-pad in fullscreen), accumulated so a burst of presses
        // becomes one seek.
        int seekDir = 0;
        if (in.repeat(BTN_R)) seekDir = 1;
        if (in.repeat(BTN_L)) seekDir = -1;
        if (full_ && in.repeat(BTN_RIGHT)) seekDir = 1;
        if (full_ && in.repeat(BTN_LEFT)) seekDir = -1;
        if (seekDir && player_) {
            double base = seekPending_ ? seekTarget_ : player_->position();
            double dur = shownDur();
            seekTarget_ = std::max(0.0, base + seekDir * 10.0);
            if (dur > 0) seekTarget_ = std::min(seekTarget_, std::max(0.0, dur - 1));
            seekPending_ = true;
            seekAt_ = now + 0.45;
            lastInteract_ = now;
        }
        if (seekPending_ && now >= seekAt_ && !in.held(BTN_L | BTN_R | BTN_LEFT | BTN_RIGHT)) {
            seekPending_ = false;
            if (player_) player_->seek(seekTarget_);
        }

        if (full_) {
            if (in.pressed(BTN_CONFIRM)) togglePause();
            if (in.tapped) {
                in.consumeTap();
                bool vis = overlayVisible(now);
                if (in.tapY > SCREEN_H - 70 && vis) seekToTap(in.tapX, barRect(FULL));
                else videoTap(FULL, in.tapX, now);
                lastInteract_ = now;
            }
            return;
        }

        handleTaps(in, now);
        switch (focus_) {
            case F_PLAYER: updatePlayerFocus(in); break;
            case F_ACTIONS: updateActions(in); break;
            case F_DESC: updateDesc(in); break;
            case F_CHIPS: updateChips(in); break;
            case F_PANEL: updatePanel(in); break;
        }
    }

    // ------------------------------------------------------------ drawing

    void draw() override {
        double now = monotonic_seconds();
        if (full_) {
            g_gfx.rect(0, 0, SCREEN_W, SCREEN_H, rgba(0, 0, 0));
            drawVideo(FULL, now);
            drawCountdown();
            return;
        }
        drawVideo(VIDEO, now);
        drawDetails();
        drawPanel();
        drawCountdown();
    }

private:
    enum Focus { F_PLAYER, F_ACTIONS, F_DESC, F_CHIPS, F_PANEL };
    enum ActionKind { A_CHANNEL, A_SUB, A_LIKE, A_DISLIKE, A_SHARE, A_SAVE, A_MORE };
    struct Action {
        ActionKind kind;
        Rect r;
    };

    // ------------------------------------------------------------ loading

    void load() {
        std::string id = item_.id;
        async<PlayerInfo>([id] { return g_app->yt.player(id); }, [this](PlayerInfo& p) { onPlayer(p); });
        async<WatchInfo>([id] { return g_app->yt.next(id); }, [this](WatchInfo& w) { onNext(w); });
    }

    void onPlayer(PlayerInfo& p) {
        info_ = p;
        infoLoaded_ = true;
        if (!p.title.empty()) item_.title = p.title;
        if (!p.author.empty()) item_.channel = p.author;
        if (!p.channelId.empty()) item_.channelId = p.channelId;
        if (p.lengthSeconds > 0) item_.duration = format_duration(p.lengthSeconds);
        if (p.viewCount >= 0 && item_.views.empty()) item_.views = format_views(p.viewCount);
        rebuildInfo();
        if (!p.ok()) {
            playError_ = friendly_status(p);
            return;
        }
        if (p.live) {
            playError_ = "Live streams aren't supported yet.";
            return;
        }
        InnerTube& yt = g_app->yt;
        if (!pick_streams(p, g_app->store.settings.maxHeight, yt.videoItags(), yt.audioItags(), vfmt_, afmt_)) {
            playError_ = "YouTube didn't offer a stream this system can decode for this video.";
            return;
        }
        haveStreams_ = true;
        double at = 0;
        if (g_app->store.settings.rememberPosition) at = g_app->store.resumePosition(item_.id);
        startPlayer(at);
        if (at > 0) g_app->toast("Resuming from " + format_duration((int)at));
        record(at);
        if (capsOn_) {
            int t = pickCaption(g_app->store.settings.captionLang);
            if (t >= 0) loadCaptions(t);
            else capsOn_ = false;
        }
    }

    void onNext(WatchInfo& w) {
        watch_ = w;
        watchLoaded_ = true;
        related_.loading = false;
        related_.error = w.error;
        Page pg;
        pg.items = w.related;
        pg.continuation = w.relatedContinuation;
        related_.append(pg);
        if (item_.channelId.empty()) item_.channelId = w.channelId;
        if (item_.channel.empty()) item_.channel = w.channelName;
        if (item_.avatar.empty()) item_.avatar = w.channelAvatar;
        item_.verified = w.verified;
        if (commentsRequested_) comments_.start(w.commentsContinuation, w.commentSorts, w.commentCount);
        if (!w.chapters.empty()) {
            // "Chapters" goes before "About"; keep the current tab selected.
            size_t at = tabs_.size() - 1;
            tabs_.insert(tabs_.begin() + at, TAB_CHAPTERS);
            if (tab_ >= (int)at) tab_++;
            chapterList_.count = (int)w.chapters.size();
        }
        rebuildInfo();
        syncRating();
    }

    // When signed in, the account's like / dislike wins over the local one.
    void syncRating() {
        if (!g_app->account.signedIn()) return;
        struct R {
            bool ok = false;
            int rating = 0;
        };
        std::string id = item_.id;
        async<R>([id] {
            R r;
            std::string err;
            r.ok = g_app->account.getRating(id, r.rating, &err);
            return r;
        }, [this](R& r) {
            if (r.ok && r.rating != g_app->store.rating(item_.id)) {
                g_app->store.setRating(item_, r.rating);
                g_app->store.saveRatings();
            }
        });
    }

    // Index of the chapter playing at `pos` seconds, or -1.
    int chapterAt(double pos) const {
        int c = -1;
        for (size_t i = 0; i < watch_.chapters.size(); i++)
            if (watch_.chapters[i].startMs <= pos * 1000.0 + 1) c = (int)i;
        return c;
    }

    void seekBy(double d) {
        if (!player_) return;
        double now = monotonic_seconds();
        double base = seekPending_ ? seekTarget_ : player_->position();
        double dur = shownDur();
        seekTarget_ = std::max(0.0, base + d);
        if (dur > 0) seekTarget_ = std::min(seekTarget_, std::max(0.0, dur - 1));
        seekPending_ = true;
        seekAt_ = now + 0.45;
        lastInteract_ = now;
    }

    // Taps on the picture: the sides wait briefly for a second tap (double tap seeks 10 s).
    void videoTap(const Rect& v, float x, double now) {
        float third = v.w / 3;
        int side = x < v.x + third ? -1 : x > v.x + 2 * third ? 1 : 0;
        if (side != 0 && pendingTap_ && pendingSide_ == side && now < pendingTapAt_) {
            pendingTap_ = false;
            seekBy(side * 10.0);
            flashSide_ = side;
            flashAt_ = now;
            return;
        }
        if (side != 0) {
            pendingTap_ = true;
            pendingSide_ = side;
            pendingTapAt_ = now + 0.3;
            return;
        }
        pendingTap_ = false;
        singleTap(now);
    }

    void singleTap(double now) {
        bool vis = overlayVisible(now - 0.3);
        lastInteract_ = now;
        if (vis) togglePause();
    }

    void startPlayer(double at) {
        stopPlayer();
        playError_.clear();
        player_ = std::make_unique<Player>();
        if (!player_->start(vfmt_, afmt_, g_app->yt.playerUserAgent(), info_.lengthSeconds, at)) {
            playError_ = player_->error().empty() ? "Couldn't start the player." : player_->error();
            player_.reset();
        }
        if (player_) player_->setSpeed(g_speed);
        ended_ = false;
    }

    void stopPlayer() {
        if (!player_) return;
        double pos = player_->position();
        bool ended = player_->state() == Player::State::Ended;
        record(ended ? info_.lengthSeconds : pos);
        g_app->store.saveHistory();
        player_->stop();
        player_.reset();
    }

    void record(double pos) {
        if (!infoLoaded_ || !info_.ok()) return;
        Item it = item_;
        if (it.avatar.empty()) it.avatar = watch_.channelAvatar;
        g_app->store.recordWatch(it, (int)pos, info_.lengthSeconds);
    }

    void tickProgress(double now, Player::State st) {
        if (!player_) return;
        if (st == Player::State::Playing && now - lastRecord_ > 5) {
            lastRecord_ = now;
            record(player_->position());
        }
        if (now - lastSave_ > 10) {
            lastSave_ = now;
            g_app->store.saveHistory();
        }
        if (st == Player::State::Ended && !ended_ && loop_) {
            record(info_.lengthSeconds);
            player_->seek(0);
            player_->setPaused(false);
            return;
        }
        if (st == Player::State::Ended && !ended_) {
            ended_ = true;
            record(info_.lengthSeconds);
            g_app->store.saveHistory();
            if (g_app->store.settings.autoplay && qi_ >= 0 && qi_ + 1 < (int)queue_.size()) {
                countdown_ = true;
                nextAt_ = now + 8;
                countdownSel_ = 1;
            }
        }
    }

    // Returns true when the countdown consumed the input this frame.
    bool handleCountdown(Input& in, double now) {
        if (!countdown_) return false;
        if (in.pressed(BTN_LEFT)) countdownSel_ = 0;
        if (in.pressed(BTN_RIGHT)) countdownSel_ = 1;
        bool cancel = in.pressed(BTN_CANCEL) || (in.pressed(BTN_CONFIRM) && countdownSel_ == 0) ||
                      in.tapIn(cdCancel_.x, cdCancel_.y, cdCancel_.w, cdCancel_.h);
        bool play = (in.pressed(BTN_CONFIRM) && countdownSel_ == 1) ||
                    in.tapIn(cdPlay_.x, cdPlay_.y, cdPlay_.w, cdPlay_.h) || now >= nextAt_;
        in.consume(BTN_CANCEL | BTN_CONFIRM | BTN_LEFT | BTN_RIGHT | BTN_UP | BTN_DOWN);
        if (cancel) {
            countdown_ = false;
            in.consumeTap();
            return true;
        }
        if (play) {
            countdown_ = false;
            in.consumeTap();
            playQueue(qi_ + 1);
            return true;
        }
        return true;
    }

    void playQueue(int i, bool mini = false) {
        if (i < 0 || i >= (int)queue_.size()) return;
        std::vector<Item> q = queue_;
        if (mini) g_app->playInMini(q[i], q, i);
        else g_app->openVideo(q[i], q, i);  // replaces this screen
    }

    void togglePause() {
        lastInteract_ = monotonic_seconds();
        if (!player_) {
            if (haveStreams_) startPlayer(0);
            return;
        }
        Player::State st = player_->state();
        if (st == Player::State::Ended) {
            countdown_ = false;
            ended_ = false;
            player_->seek(0);
            player_->setPaused(false);
            return;
        }
        player_->setPaused(!player_->paused());
    }

    void toggleFull() {
        full_ = !full_;
        lastInteract_ = monotonic_seconds();
    }

    void seekToTap(float x, const Rect& bar) {
        if (!player_) return;
        double dur = shownDur();
        if (dur <= 0) return;
        double f = std::max(0.0, std::min(1.0, (double)(x - bar.x) / bar.w));
        player_->seek(std::min(dur - 1, f * dur));
        seekPending_ = false;
    }

    bool overlayVisible(double now) const {
        if (!player_) return true;
        Player::State st = player_->state();
        return st != Player::State::Playing || now - lastInteract_ < 3.0 || seekPending_;
    }

    // ------------------------------------------------------------ focus handling

    void handleTaps(Input& in, double now) {
        if (!in.tapped) return;
        float x = in.tapX, y = in.tapY;
        if (VIDEO.contains(x, y)) {
            in.consumeTap();
            focus_ = F_PLAYER;
            bool vis = overlayVisible(now);
            lastInteract_ = now;
            if (vis && y > VIDEO.h - 44) {
                if (x > VIDEO.w - 46) toggleFull();
                else if (x > VIDEO.w - 86) optionsMenu();
                else if (x > VIDEO.w - 126 && !info_.captions.empty()) captionsMenu();
                else seekToTap(x, barRect(VIDEO));
            } else {
                videoTap(VIDEO, x, now);
            }
            return;
        }
        for (size_t i = 0; i < actions_.size(); i++) {
            const Rect& r = actions_[i].r;
            if (in.tapIn(r.x - 4, r.y - 4, r.w + 8, r.h + 8)) {
                in.consumeTap();
                focus_ = F_ACTIONS;
                actionSel_ = (int)i;
                runAction(actions_[i].kind);
                return;
            }
        }
        if (descBox_.contains(x, y)) {
            in.consumeTap();
            openAbout();
            return;
        }
        if (y < PANEL_LIST.y && x >= PANEL_X) {
            int t = chips_hit(in, chipNames(), chipsX(), CHIPS_Y, kChipScale, kChipGap);
            in.consumeTap();
            if (t >= 0) setTab(t);
        }
    }

    void updatePlayerFocus(Input& in) {
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            togglePause();
        }
        if (in.pressed(BTN_RIGHT)) focus_ = F_PANEL;
        if (in.pressed(BTN_DOWN)) {
            focus_ = F_ACTIONS;
            actionSel_ = 0;
        }
    }

    void updateActions(Input& in) {
        int n = (int)actions_.size();
        if (n == 0) {
            focus_ = F_PLAYER;
            return;
        }
        actionSel_ = std::max(0, std::min(actionSel_, n - 1));
        if (in.repeat(BTN_RIGHT)) {
            if (actionSel_ + 1 < n) actionSel_++;
            else if (in.pressed(BTN_RIGHT)) focus_ = F_PANEL;
        }
        if (in.repeat(BTN_LEFT) && actionSel_ > 0) actionSel_--;
        if (in.pressed(BTN_UP)) focus_ = F_PLAYER;
        if (in.pressed(BTN_DOWN)) focus_ = F_DESC;
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            runAction(actions_[actionSel_].kind);
        }
    }

    void updateDesc(Input& in) {
        if (in.pressed(BTN_UP)) focus_ = F_ACTIONS;
        if (in.pressed(BTN_RIGHT)) focus_ = F_PANEL;
        if (in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            openAbout();
        }
    }

    void updateChips(Input& in) {
        if (in.repeat(BTN_RIGHT) && tab_ + 1 < (int)tabs_.size()) setTab(tab_ + 1, false);
        if (in.repeat(BTN_LEFT)) {
            if (tab_ > 0) setTab(tab_ - 1, false);
            else if (in.pressed(BTN_LEFT)) focus_ = F_PLAYER;
        }
        if (in.pressed(BTN_DOWN) || in.pressed(BTN_CONFIRM)) {
            in.consume(BTN_CONFIRM);
            focus_ = F_PANEL;
        }
    }

    void runAction(ActionKind k) {
        int r = g_app->store.rating(item_.id);
        switch (k) {
            case A_CHANNEL: g_app->openChannel(item_.channelId, item_.channel); break;  // keeps playing small
            case A_SUB: g_app->toggleSubscription(item_.channelId, item_.channel, channelAvatar()); break;
            case A_LIKE: g_app->rateVideo(item_, r == 1 ? 0 : 1); break;
            case A_DISLIKE: g_app->rateVideo(item_, r == -1 ? 0 : -1); break;
            case A_SHARE: g_app->shareItem(item_); break;
            case A_SAVE: g_app->saveToPlaylist(item_); break;
            case A_MORE: optionsMenu(); break;
        }
    }

    void openAbout() {
        for (size_t i = 0; i < tabs_.size(); i++)
            if (tabs_[i] == TAB_INFO) setTab((int)i);
        focus_ = F_PANEL;
    }

    std::string channelAvatar() const { return !watch_.channelAvatar.empty() ? watch_.channelAvatar : item_.avatar; }

    // ------------------------------------------------------------ captions

    int pickCaption(const std::string& lang) const {
        const auto& c = info_.captions;
        for (int pass = 0; pass < 3; pass++) {
            for (size_t i = 0; i < c.size(); i++) {
                bool exact = c[i].lang == lang;
                bool prefix = starts_with(c[i].lang, (lang + "-").c_str()) || starts_with(lang, (c[i].lang + "-").c_str());
                if (pass == 0 && exact && !c[i].asr) return (int)i;
                if (pass == 1 && exact) return (int)i;
                if (pass == 2 && prefix) return (int)i;
            }
        }
        return -1;
    }

    void loadCaptions(int track) {
        if (track < 0 || track >= (int)info_.captions.size()) return;
        capTrack_ = track;
        capsOn_ = true;
        caps_ = Captions();
        std::string url = info_.captions[track].url;
        struct R {
            bool ok = false;
            Captions caps;
            std::string err;
        };
        int gen = ++capGen_;
        async<R>([url] {
            R r;
            std::string body;
            if (g_app->yt.captions(url, body, &r.err)) r.ok = r.caps.parse(body, &r.err);
            return r;
        }, [this, gen](R& r) {
            if (gen != capGen_) return;
            if (r.ok) caps_ = std::move(r.caps);
            else g_app->toast("Couldn't load captions: " + r.err);
        });
    }

    void drawCaptions(const Rect& v, double pos, bool controls) {
        if (!capsOn_ || caps_.size() == 0) return;
        std::string t = caps_.at(pos);
        if (t.empty()) return;
        float scale = full_ ? 0.95f : 0.74f;
        const auto& lines = g_gfx.wrap(t, v.w * 0.84f, scale, 3);
        float lh = g_gfx.lineHeight(scale) + 2;
        float y = v.y + v.h - (controls ? (full_ ? 84 : 58) : 24) - lines.size() * lh;
        for (size_t i = 0; i < lines.size(); i++) {
            float w = g_gfx.width(lines[i], scale) + 16;
            g_gfx.rect(v.x + (v.w - w) / 2, y + i * lh, w, lh, rgba(8, 8, 8, 192));
            g_gfx.textCentered(v.x + v.w / 2, y + i * lh + 1, col::text, scale, lines[i]);
        }
    }

    // ------------------------------------------------------------ menus

    std::vector<int> qualityHeights() const {
        std::set<int> hs;
        for (const Format& f : info_.formats)
            if (f.isVideo() && f.isAvc() && f.hasRanges() && f.width * f.height <= 1280 * 720 && quality_of(f) > 0)
                hs.insert(quality_of(f));
        return std::vector<int>(hs.rbegin(), hs.rend());
    }

    void optionsMenu() {
        std::vector<MenuAction> acts;
        if (haveStreams_)
            acts.push_back({"Quality  " + str_printf("%dp", quality_of(vfmt_)), Icon::Settings, guarded([this] { qualityMenu(); })});
        if (haveStreams_)
            acts.push_back({"Playback speed  " + speed_label(g_speed), Icon::Speed, guarded([this] { speedMenu(); })});
        if (!info_.captions.empty()) {
            std::string cur = capsOn_ && capTrack_ >= 0 ? captionName(capTrack_) : "Off";
            acts.push_back({"Captions  " + cur, Icon::Captions, guarded([this] { captionsMenu(); })});
        }
        acts.push_back({std::string("Loop video  ") + (loop_ ? "On" : "Off"), Icon::Loop, guarded([this] {
                            loop_ = !loop_;
                            g_app->toast(loop_ ? "Loop is on" : "Loop is off");
                        })});
        if (!watch_.chapters.empty()) acts.push_back({"Chapters", Icon::Playlist, guarded([this] { chaptersMenu(); })});
        acts.push_back({full_ ? "Exit full screen" : "Full screen", Icon::Fullscreen, guarded([this] { toggleFull(); })});
        Item copy = item_;
        acts.push_back({"Save to playlist", Icon::Save, [copy] { g_app->saveToPlaylist(copy); }});
        acts.push_back({"Share", Icon::Share, [copy] { g_app->shareItem(copy); }});
        std::string cid = item_.channelId, cname = item_.channel, avatar = channelAvatar();
        if (!cid.empty()) {
            bool sub = g_app->store.isSubscribed(cid);
            acts.push_back({(sub ? "Unsubscribe from " : "Subscribe to ") + cname, Icon::Subscriptions,
                            [cid, cname, avatar] { g_app->toggleSubscription(cid, cname, avatar); }});
            acts.push_back({"Go to channel", Icon::Account, [cid, cname] { g_app->openChannel(cid, cname); }});
        }
        g_app->showActions("", acts);
    }

    void speedMenu() {
        std::vector<std::string> labels;
        int cur = 3;
        for (int i = 0; i < 8; i++) {
            labels.push_back(speed_label(kSpeeds[i]));
            if (kSpeeds[i] == g_speed) cur = i;
        }
        g_app->showMenu("Playback speed", labels, guarded([this](int i) {
                            g_speed = kSpeeds[i];
                            if (player_) player_->setSpeed(g_speed);
                            g_app->toast("Playback speed " + speed_label(g_speed));
                        }),
                        cur, cur);
    }

    void chaptersMenu() {
        std::vector<std::string> labels;
        for (auto& c : watch_.chapters) labels.push_back(format_duration(c.startMs / 1000) + "   " + c.title);
        int cur = std::max(0, chapterAt(shownPos()));
        g_app->showMenu("Chapters", labels, guarded([this](int i) { seekToChapter(i); }), cur, cur);
    }

    void seekToChapter(int i) {
        if (i < 0 || i >= (int)watch_.chapters.size()) return;
        double t = watch_.chapters[i].startMs / 1000.0;
        if (player_) {
            player_->seek(t);
            player_->setPaused(false);
        } else {
            resume_ = t;
        }
        seekPending_ = false;
        lastInteract_ = monotonic_seconds();
    }

    void qualityMenu() {
        std::vector<int> hs = qualityHeights();
        if (hs.empty()) {
            g_app->toast("No other qualities for this video");
            return;
        }
        std::vector<std::string> labels;
        int sel = 0;
        for (size_t i = 0; i < hs.size(); i++) {
            labels.push_back(str_printf("%dp", hs[i]));
            if (hs[i] == quality_of(vfmt_)) sel = (int)i;
        }
        g_app->showMenu("Quality for current video", labels, guarded([this, hs](int i) { switchQuality(hs[i]); }), sel,
                        sel);
    }

    void switchQuality(int height) {
        Format v, a;
        InnerTube& yt = g_app->yt;
        if (!pick_streams(info_, height, yt.videoItags(), yt.audioItags(), v, a)) return;
        if (v.itag == vfmt_.itag) return;
        double pos = player_ ? player_->position() : resume_;
        bool wasPaused = player_ && player_->paused();
        vfmt_ = v;
        afmt_ = a;
        startPlayer(pos);
        if (wasPaused && player_) player_->setPaused(true);
        g_app->toast(str_printf("Quality set to %dp", quality_of(v)));
    }

    std::string captionName(int i) const {
        const CaptionTrack& t = info_.captions[i];
        std::string n = t.name.empty() ? t.lang : t.name;
        return n;
    }

    void captionsMenu() {
        if (info_.captions.empty()) {
            g_app->toast("Captions aren't available for this video");
            return;
        }
        std::vector<std::string> labels{"Off"};
        for (size_t i = 0; i < info_.captions.size(); i++) labels.push_back(captionName((int)i));
        int sel = capsOn_ && capTrack_ >= 0 ? capTrack_ + 1 : 0;
        g_app->showMenu("Subtitles/CC", labels, guarded([this](int i) {
            // Like YouTube, the choice sticks: later videos open with captions on (or off).
            Settings& st = g_app->store.settings;
            if (i == 0) {
                capsOn_ = false;
                capGen_++;
                g_app->toast("Captions off");
                if (st.captions) {
                    st.captions = false;
                    g_app->store.saveSettings();
                }
                return;
            }
            loadCaptions(i - 1);
            const std::string& lang = info_.captions[i - 1].lang;
            auto base = [](const std::string& s) { return s.substr(0, s.find('-')); };
            bool changed = !st.captions;
            st.captions = true;
            if (!lang.empty() && base(lang) != base(st.captionLang)) {
                st.captionLang = lang;
                changed = true;
            }
            if (changed) g_app->store.saveSettings();
        }), sel, sel);
    }

    // ------------------------------------------------------------ side panel

    std::vector<std::string> chipNames() const {
        std::vector<std::string> v;
        for (PanelTab t : tabs_) v.push_back(tabName(t));
        return v;
    }

    // Chips scroll so the current one stays inside the column.
    float chipsX() const {
        std::vector<std::string> names = chipNames();
        float x = 0, right = 0;
        for (int i = 0; i < (int)names.size(); i++) {
            float w = chip_width(names[i], kChipScale);
            if (i == tab_) right = x + w;
            x += w + kChipGap;
        }
        float avail = PANEL_W - 12;
        return PANEL_X + std::min(0.0f, avail - right);
    }

    static const char* tabName(PanelTab t) {
        switch (t) {
            case TAB_QUEUE: return "Up next";
            case TAB_RELATED: return "Related";
            case TAB_COMMENTS: return "Comments";
            case TAB_CHAPTERS: return "Chapters";
            default: return "About";
        }
    }

    void setTab(int t, bool focusList = true) {
        tab_ = std::max(0, std::min(t, (int)tabs_.size() - 1));
        if (focusList) focus_ = F_PANEL;
        PanelTab pt = tabs_[tab_];
        if (pt == TAB_COMMENTS && !commentsRequested_) {
            commentsRequested_ = true;
            if (watchLoaded_) comments_.start(watch_.commentsContinuation, watch_.commentSorts, watch_.commentCount);
        }
    }

    void hiddenChanged() override { related_.prune(); }

    const Item* panelSelectedItem() const {
        PanelTab t = tabs_[tab_];
        if (t == TAB_QUEUE) return queueFeed_.selected();
        if (t == TAB_RELATED) return related_.selected();
        return nullptr;
    }

    std::string metaText() const {
        std::string meta = !watch_.viewsText.empty() ? watch_.viewsText : item_.views;
        if (!watch_.dateText.empty()) meta += (meta.empty() ? "" : "  ") + watch_.dateText;
        return meta;
    }

    void rebuildInfo() {
        desc_.lines.clear();
        desc_.bold.clear();
        auto add = [&](const std::string& text, float scale, bool bold, int maxLines) {
            for (const std::string& l : g_gfx.wrap(text, PANEL_W - 28, scale, maxLines)) {
                desc_.lines.push_back(l);
                desc_.bold.push_back(bold);
            }
        };
        add(item_.title, 0.7f, true, 4);
        add(metaText(), 0.7f, true, 2);
        desc_.lines.push_back("");
        desc_.bold.push_back(false);
        add(info_.description.empty() ? "No description has been added to this video." : info_.description, 0.7f, false, 600);
        desc_.list.rowH = g_gfx.lineHeight(0.7f);
        desc_.list.count = (int)desc_.lines.size();
    }

    void updatePanel(Input& in) {
        PanelTab t = tabs_[tab_];
        bool atTop = false;
        if (t == TAB_QUEUE) {
            queueFeed_.setFocused(true);
            atTop = queueFeed_.items.empty() || queueFeed_.sel() == 0;
            int a = queueFeed_.update(in, PANEL_LIST);
            if (a >= 0) {
                if (a == qi_) togglePause();
                else playQueue(a);
                return;
            }
        } else if (t == TAB_RELATED) {
            related_.setFocused(true);
            atTop = related_.items.empty() || related_.sel() == 0;
            int a = related_.update(in, PANEL_LIST);
            if (a >= 0) {
                g_app->openItem(related_.items[a]);
                return;
            }
            if (related_.wantsMore(PANEL_LIST)) moreRelated();
            if (related_.takeRetry()) {
                related_.reset();
                related_.loading = true;
                load();
            }
        } else if (t == TAB_COMMENTS) {
            atTop = !comments_.started() || comments_.headerFocused();
            if (comments_.started()) comments_.update(in, PANEL_LIST, true);
        } else if (t == TAB_CHAPTERS) {
            chapterList_.focused = true;
            chapterList_.count = (int)watch_.chapters.size();
            atTop = chapterList_.sel == 0;
            int a = chapterList_.update(in, PANEL_LIST);
            if (a >= 0) seekToChapter(a);
        } else {
            // The description scrolls a page at a time rather than selecting lines.
            desc_.list.focused = false;
            atTop = desc_.list.scroll() <= 0.5f;
            float page = PANEL_LIST.h * 0.6f;
            if (in.repeat(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                desc_.list.scrollBy(PANEL_LIST, page);
            }
            if (in.repeat(BTN_UP) && !atTop) {
                in.consume(BTN_UP);
                desc_.list.scrollBy(PANEL_LIST, -page);
            }
            desc_.list.update(in, PANEL_LIST);
        }
        if (in.pressed(BTN_UP) && atTop) {
            in.consume(BTN_UP);
            focus_ = F_CHIPS;
        }
        if (in.pressed(BTN_LEFT)) {
            in.consume(BTN_LEFT);
            focus_ = F_PLAYER;
        }
        if (in.pressed(BTN_CONFIRM)) in.consume(BTN_CONFIRM);
    }

    void moreRelated() {
        related_.loading = true;
        std::string cont = related_.continuation;
        uint32_t gen = related_.gen;
        async<WatchInfo>([cont] { return g_app->yt.relatedMore(cont); }, [this, gen](WatchInfo& w) {
            if (gen != related_.gen) return;
            related_.loading = false;
            if (!w.error.empty()) {
                related_.error = w.error;
                return;
            }
            Page pg;
            pg.items = w.related;
            pg.continuation = w.relatedContinuation;
            related_.append(pg);
        });
    }

    void drawPanel() {
        bool chipFoc = focus_ == F_CHIPS;
        g_gfx.clip(Rect{PANEL_X - 6, 0, PANEL_W + 6, PANEL_LIST.y});
        draw_chips(chipNames(), tab_, chipFoc ? tab_ : -1, chipsX(), CHIPS_Y, kChipScale, kChipGap);
        g_gfx.unclip();
        bool foc = focus_ == F_PANEL;
        PanelTab t = tabs_[tab_];
        if (t == TAB_QUEUE) {
            queueFeed_.setFocused(foc);
            queueFeed_.list.count = (int)queueFeed_.items.size();
            queueFeed_.list.draw(PANEL_LIST, [&](int i, const Rect& r, bool sel) {
                draw_compact_row(queueFeed_.items[i], r, sel, i, i == qi_);
            });
        } else if (t == TAB_RELATED) {
            related_.setFocused(foc);
            related_.draw(PANEL_LIST);
        } else if (t == TAB_COMMENTS) {
            if (!comments_.started()) draw_loading(PANEL_LIST);
            else comments_.draw(PANEL_LIST, foc);
        } else if (t == TAB_CHAPTERS) {
            drawChapters(foc);
        } else {
            desc_.list.focused = false;
            desc_.list.draw(PANEL_LIST, [&](int i, const Rect& r, bool) {
                g_gfx.textLine(r.x + 14, r.y, PANEL_W - 28, col::text, 0.7f, desc_.lines[i],
                               desc_.bold[i] ? W_MEDIUM : W_REGULAR);
            });
            if (foc) g_gfx.rect(PANEL_X - 4, PANEL_LIST.y, 2, PANEL_LIST.h, col::focus);
        }
    }

    void drawChapters(bool foc) {
        int cur = chapterAt(shownPos());
        chapterList_.focused = foc;
        chapterList_.count = (int)watch_.chapters.size();
        chapterList_.draw(PANEL_LIST, [&](int i, const Rect& r, bool sel) {
            const Chapter& c = watch_.chapters[i];
            uint32_t bg = sel ? col::rowHi : i == cur ? col::rowCur : col::bg;
            if (sel || i == cur) g_gfx.roundRect(r.x + 4, r.y + 2, r.w - 12, r.h - 4, 10, bg);
            // Thumbnail-sized time badge, then the title.
            float bx = r.x + 14, by = r.y + 9, bw = 68, bh = r.h - 18;
            g_gfx.roundRect(bx, by, bw, bh, 6, i == cur ? col::red : col::raised);
            g_gfx.textCentered(bx + bw / 2, by + (bh - g_gfx.lineHeight(0.62f)) / 2, col::text, 0.62f,
                               format_duration(c.startMs / 1000), W_MEDIUM);
            g_gfx.paragraph(bx + bw + 12, r.y + 8, r.w - bw - 44, col::text, 0.64f, c.title, 2, 1.0f,
                            i == cur ? W_MEDIUM : W_REGULAR);
        });
    }

    // ------------------------------------------------------------ video + details

    double shownPos() const {
        if (seekPending_) return seekTarget_;
        return player_ ? player_->position() : resume_;
    }
    double shownDur() const {
        double d = player_ ? player_->duration() : 0;
        return d > 0 ? d : info_.lengthSeconds;
    }
    static Rect barRect(const Rect& v) {
        bool full = v.w > 700;
        float pad = full ? 24 : 12;
        return Rect{v.x + pad, v.y + v.h - (full ? 52 : 38), v.w - 2 * pad, 4};
    }
    std::string timeText() const {
        return format_duration((int)shownPos()) + " / " + format_duration((int)shownDur());
    }

    void drawVideo(const Rect& v, double now) {
        g_gfx.rect(v.x, v.y, v.w, v.h, rgba(0, 0, 0));
        double pos = 0;
        if (player_) {
            player_->draw(v.x, v.y, v.w, v.h);
            pos = player_->position();
        } else if (!item_.id.empty()) {
            if (vita2d_texture* t = g_app->thumbs.get(item_.thumbUrl())) g_gfx.imageCover(t, v.x, v.y, v.w, v.h);
            g_gfx.rect(v.x, v.y, v.w, v.h, rgba(0, 0, 0, 110));
        }
        std::string err = !playError_.empty() ? playError_
                                              : (player_ && player_->state() == Player::State::Error ? player_->error() : "");
        if (!err.empty()) {
            g_gfx.rect(v.x, v.y, v.w, v.h, rgba(0, 0, 0, 200));
            draw_empty_state(Rect{v.x + 20, v.y, v.w - 40, v.h}, Icon::Info, "Video unavailable", err);
            return;
        }
        bool controls = overlayVisible(now) && !countdown_;
        drawCaptions(v, pos, controls);
        float cx = v.x + v.w / 2, cy = v.y + v.h / 2;
        Player::State st = player_ ? player_->state() : Player::State::Loading;
        bool busy = st == Player::State::Loading || st == Player::State::Buffering || !infoLoaded_;
        if (controls) drawControls(v, st, busy);
        else drawThinProgress(v);
        if (busy && !seekPending_) g_gfx.spinner(cx, cy, full_ ? 26 : 20, col::text);
        if (now - flashAt_ < 0.6) {
            float k = (float)(1 - (now - flashAt_) / 0.6);
            float fx = flashSide_ < 0 ? v.x + v.w * 0.17f : v.x + v.w * 0.83f;
            g_gfx.circle(fx, cy, v.h * 0.3f, rgba(255, 255, 255, (uint8_t)(40 * k)));
            const char* arrows = flashSide_ < 0 ? "\xE2\x97\x80\xE2\x97\x80" : "\xE2\x96\xB6\xE2\x96\xB6";
            g_gfx.textCentered(fx, cy - 26, with_alpha(col::text, (uint8_t)(255 * k)), 0.8f, arrows);
            g_gfx.textCentered(fx, cy + 2, with_alpha(col::text, (uint8_t)(255 * k)), 0.66f, "10 seconds", W_MEDIUM);
        }
        if (seekPending_) {
            std::string s = format_duration((int)seekTarget_);
            float w = g_gfx.width(s, 1.0f, true) + 36;
            g_gfx.roundRect(cx - w / 2, cy - 24, w, 48, 24, rgba(0, 0, 0, 160));
            g_gfx.textCentered(cx, cy - g_gfx.lineHeight(1.0f) / 2, col::text, 1.0f, s, true);
        }
    }

    void drawThinProgress(const Rect& v) {
        double dur = shownDur();
        if (dur <= 0) return;
        float f = (float)std::min(1.0, shownPos() / dur);
        g_gfx.rect(v.x, v.y + v.h - 3, v.w, 3, rgba(255, 255, 255, 50));
        g_gfx.rect(v.x, v.y + v.h - 3, v.w * f, 3, col::red);
    }

    void drawControls(const Rect& v, Player::State st, bool busy) {
        bool full = full_;
        // Scrims behind the controls.
        float bh = full ? 110 : 84;
        g_gfx.vgradient(v.x, v.y + v.h - bh, v.w, bh, rgba(0, 0, 0, 0), rgba(0, 0, 0, 200));
        if (full) {
            g_gfx.vgradient(v.x, v.y, v.w, 70, rgba(0, 0, 0, 190), rgba(0, 0, 0, 0));
            g_gfx.textBold(v.x + 24, v.y + 18, col::text, 0.92f, g_gfx.ellipsize(item_.title, v.w - 48, 0.92f));
        }
        // Big play / pause in the middle.
        float cx = v.x + v.w / 2, cy = v.y + v.h / 2;
        if (!busy && !seekPending_ && st != Player::State::Ended) {
            float r = full ? 36 : 30;
            g_gfx.circle(cx, cy, r, rgba(0, 0, 0, 120));
            bool paused = st == Player::State::Paused;
            draw_icon(paused ? Icon::Play : Icon::Pause, cx + (paused ? 2 : 0), cy, r * 1.1f, col::text, rgba(0, 0, 0, 120));
        } else if (st == Player::State::Ended) {
            g_gfx.roundRect(cx - 70, cy - 22, 140, 44, 22, rgba(0, 0, 0, 160));
            g_gfx.textCentered(cx, cy - g_gfx.lineHeight(0.8f) / 2, col::text, 0.8f, C() + "  Replay", true);
        }
        // Seek bar: played (red to pink), buffered (light), rest (dim), chapter gaps, scrubber.
        Rect bar = barRect(v);
        double dur = shownDur(), pos = shownPos();
        g_gfx.rect(bar.x, bar.y, bar.w, bar.h, rgba(255, 255, 255, 60));
        if (dur > 0) {
            double buf = player_ ? std::min(dur, player_->position() + player_->bufferedAhead()) : 0;
            g_gfx.rect(bar.x, bar.y, (float)(bar.w * buf / dur), bar.h, rgba(255, 255, 255, 110));
            float px = (float)(bar.w * std::min(1.0, pos / dur));
            g_gfx.hgradient(bar.x, bar.y, px, bar.h, col::red, col::redPink);
            for (size_t i = 1; i < watch_.chapters.size(); i++) {
                float cx = bar.x + (float)(bar.w * std::min(1.0, watch_.chapters[i].startMs / 1000.0 / dur));
                g_gfx.rect(cx - 1, bar.y - 1, 2, bar.h + 2, rgba(0, 0, 0, 200));
            }
            bool knob = full || focus_ == F_PLAYER || seekPending_;
            if (knob) g_gfx.circle(bar.x + px, bar.y + bar.h / 2, 7, col::red);
        }
        // Bottom row: play/pause + time on the left, CC / quality / full screen on the right.
        float rowY = bar.y + 20;
        float lx = bar.x + 4;
        bool paused = !player_ || st == Player::State::Paused || st == Player::State::Ended;
        draw_icon(paused ? Icon::Play : Icon::Pause, lx + 9, rowY, 18, col::text, rgba(0, 0, 0, 190));
        std::string tt = timeText();
        g_gfx.text(lx + 32, rowY - g_gfx.lineHeight(0.64f) / 2, col::text, 0.64f, tt);
        float rx = bar.x + bar.w - 6;
        draw_icon(Icon::Fullscreen, rx - 9, rowY, 18, col::text, rgba(0, 0, 0, 190));
        rx -= 40;
        draw_icon(Icon::Settings, rx - 10, rowY, 19, col::text, rgba(0, 0, 0, 190));
        if (haveStreams_ && quality_of(vfmt_) >= 720) {  // "HD" badge on the gear, as on the site
            g_gfx.roundRect(rx - 4, rowY - 13, 14, 9, 2, col::red);
            g_gfx.textCentered(rx + 3, rowY - 13 + (9 - g_gfx.lineHeight(0.36f)) / 2, col::text, 0.36f, "HD", W_BOLD);
        }
        rx -= 40;
        if (!info_.captions.empty()) {
            draw_icon(Icon::Captions, rx - 12, rowY, 24, col::text, rgba(0, 0, 0, 190));
            if (capsOn_) g_gfx.rect(rx - 22, rowY + 11, 20, 2, col::red);
            rx -= 40;
        }
        if (loop_) draw_icon(Icon::Loop, rx - 10, rowY, 18, col::text, rgba(0, 0, 0, 190));
        // Current chapter after the time: "  ·  Intro  >".
        int ch = chapterAt(pos);
        if (ch >= 0) {
            float cx = lx + 32 + g_gfx.width(tt, 0.64f) + 4;
            std::string t = g_gfx.ellipsize(G_DOT + watch_.chapters[ch].title, rx - 30 - cx, 0.64f);
            g_gfx.text(cx, rowY - g_gfx.lineHeight(0.64f) / 2, col::text, 0.64f, t);
            draw_icon(Icon::ChevronRight, cx + g_gfx.width(t, 0.64f) + 8, rowY, 12, col::text, rgba(0, 0, 0, 190));
        }
    }

    // Like | dislike as one segmented pill. Returns its width.
    float drawLikePill(float x, float y, int likeIdx) {
        bool act = focus_ == F_ACTIONS;
        bool fl = act && actionSel_ == likeIdx, fr = act && actionSel_ == likeIdx + 1;
        int r = g_app->store.rating(item_.id);
        std::string likes = watch_.likes.empty() ? "Like" : watch_.likes;
        const float s = 0.68f, h = kPillH;
        float lw = 14 + 20 + 8 + g_gfx.width(likes, s, W_MEDIUM) + 14, rw = 46;
        g_gfx.roundRect(x, y, lw + rw, h, h / 2, col::raised);
        if (fl) {
            g_gfx.roundRect(x, y, lw, h, h / 2, col::raisedHi);
            g_gfx.rect(x + lw - h / 2, y, h / 2, h, col::raisedHi);
        }
        if (fr) {
            g_gfx.roundRect(x + lw, y, rw, h, h / 2, col::raisedHi);
            g_gfx.rect(x + lw, y, h / 2, h, col::raisedHi);
        }
        g_gfx.rect(x + lw, y + 6, 1, h - 12, rgba(255, 255, 255, 60));
        draw_icon(r == 1 ? Icon::ThumbUpFilled : Icon::ThumbUp, x + 14 + 10, y + h / 2, 20, col::text,
                  fl ? col::raisedHi : col::raised);
        g_gfx.text(x + 14 + 28, y + (h - g_gfx.lineHeight(s)) / 2, col::text, s, likes, W_MEDIUM);
        draw_icon(r == -1 ? Icon::ThumbDownFilled : Icon::ThumbDown, x + lw + rw / 2 - 2, y + h / 2, 20, col::text,
                  fr ? col::raisedHi : col::raised);
        if (fl || fr) g_gfx.roundFrame(x - 4, y - 4, lw + rw + 8, h + 8, h / 2 + 4, 2.5f, col::focus);
        actions_.push_back({A_LIKE, Rect{x, y, lw, h}});
        actions_.push_back({A_DISLIKE, Rect{x + lw, y, rw, h}});
        return lw + rw;
    }

    void drawDetails() {
        float x = 12, w = VIDEO.w - 24;
        float y = VIDEO.h + 8;
        y += g_gfx.paragraph(x, y, w, col::text, 0.86f, item_.title.empty() ? "Loading..." : item_.title, 2, 1.0f, W_BOLD);
        y += 6;

        // Channel row: avatar, name + subscribers, Subscribe; like | dislike, Share, Save and More on the right.
        actions_.clear();
        float rowY = y, rowH = 40;
        float pillY = rowY + (rowH - kPillH) / 2;
        std::string likes = watch_.likes.empty() ? "Like" : watch_.likes;
        float likeW = 14 + 20 + 8 + g_gfx.width(likes, 0.68f, W_MEDIUM) + 14 + 46;
        float shareW = pill_width("Share", true, kPillH), saveW = pill_width("Save", true, kPillH);
        float moreW = pill_width("", true, kPillH);
        float rightW = likeW + shareW + saveW + moreW + 3 * 8;
        bool sub = !item_.channelId.empty() && g_app->store.isSubscribed(item_.channelId);
        std::string subLabel = sub ? "Subscribed" : "Subscribe";
        float subW = pill_width(subLabel, false, kPillH);

        float nx = x + 46;
        float nameMax = VIDEO.w - 12 - rightW - subW - 22 - nx;
        float isz = 11;
        std::string nm = g_gfx.ellipsize(item_.channel, nameMax - (item_.verified ? isz + 4 : 0), 0.72f, W_MEDIUM);
        float nmW = (float)g_gfx.width(nm, 0.72f, W_MEDIUM);
        std::string subs = g_gfx.ellipsize(watch_.subscribers, nameMax, 0.56f);
        float nameW = std::max(nmW + (item_.verified ? isz + 4 : 0), (float)g_gfx.width(subs, 0.56f));
        bool act = focus_ == F_ACTIONS;
        int idx = 0;
        uint32_t rowBg = col::bg;
        if (!item_.channelId.empty()) {
            Rect cr{x - 2, rowY - 2, nx - x + std::min(nameW, nameMax) + 12, rowH + 4};
            if (act && actionSel_ == idx) {
                rowBg = col::raisedHi;
                g_gfx.roundRect(cr.x, cr.y, cr.w, cr.h, cr.h / 2, rowBg);
                g_gfx.roundFrame(cr.x - 3, cr.y - 3, cr.w + 6, cr.h + 6, cr.h / 2 + 3, 2.5f, col::focus);
            }
            actions_.push_back({A_CHANNEL, cr});
            idx++;
        }
        draw_avatar(channelAvatar(), item_.channel, x + 18, rowY + rowH / 2, 18, rowBg);
        g_gfx.text(nx, rowY + 1, col::text, 0.72f, nm, W_MEDIUM);
        if (item_.verified) {
            draw_icon(Icon::Verified, nx + nmW + 4 + isz / 2, rowY + 1 + g_gfx.lineHeight(0.72f) / 2, isz, col::text2,
                      rowBg);
        }
        if (!subs.empty()) g_gfx.text(nx, rowY + 22, col::text2, 0.56f, subs);
        float sx = nx + std::min(nameW, nameMax) + 16;
        if (!item_.channelId.empty()) {
            draw_pill(sx, pillY, subLabel, sub ? PillStyle::Secondary : PillStyle::Primary, act && actionSel_ == idx, false,
                      Icon::Play, kPillH);
            actions_.push_back({A_SUB, Rect{sx, pillY, subW, kPillH}});
            idx++;
        }
        float px = VIDEO.w - 12 - rightW;
        px += drawLikePill(px, pillY, idx) + 8;
        idx += 2;
        struct Spec {
            ActionKind k;
            const char* label;
            Icon icon;
        };
        const Spec right[] = {{A_SHARE, "Share", Icon::Share}, {A_SAVE, "Save", Icon::Save}, {A_MORE, "", Icon::More}};
        for (const Spec& sp : right) {
            float pw = draw_pill(px, pillY, sp.label, PillStyle::Secondary, act && actionSel_ == idx, true, sp.icon, kPillH);
            actions_.push_back({sp.k, Rect{px, pillY, pw, kPillH}});
            px += pw + 8;
            idx++;
        }
        y = rowY + rowH + 10;

        // Description box.
        float bh = SCREEN_H - y - 8;
        descBox_ = Rect{x, y, w, bh};
        bool dfoc = focus_ == F_DESC;
        uint32_t boxBg = dfoc ? col::raisedHi : col::raised;
        g_gfx.roundRect(x, y, w, bh, 12, boxBg);
        if (dfoc) g_gfx.roundFrame(x - 4, y - 4, w + 8, bh + 8, 16, 2.5f, col::focus);
        float tx = x + 12, ty = y + 9;
        std::string meta = metaText();
        if (!meta.empty()) {
            g_gfx.text(tx, ty, col::text, 0.64f, g_gfx.ellipsize(meta, w - 24, 0.64f, W_MEDIUM), W_MEDIUM);
            ty += g_gfx.lineHeight(0.64f) + 2;
        }
        int lines = std::max(1, (int)((y + bh - 8 - ty) / g_gfx.lineHeight(0.62f)));
        std::string d = info_.description;
        std::replace(d.begin(), d.end(), '\n', ' ');
        if (!d.empty()) g_gfx.paragraph(tx, ty, w - 70, col::text, 0.62f, d, lines);
        g_gfx.textRight(x + w - 12, y + bh - 8 - g_gfx.lineHeight(0.62f), col::text, 0.62f, "...more", W_MEDIUM);
    }

    void drawCountdown() {
        if (!countdown_ || qi_ + 1 >= (int)queue_.size()) return;
        double left = std::max(0.0, nextAt_ - monotonic_seconds());
        const Item& n = queue_[qi_ + 1];
        const Rect& v = full_ ? FULL : VIDEO;
        g_gfx.rect(v.x, v.y, v.w, v.h, rgba(0, 0, 0, 170));
        float w = 440, h = 170;
        float x = v.x + (v.w - w) / 2, y = v.y + (v.h - h) / 2;
        g_gfx.text(x, y, col::text2, 0.7f, str_printf("Up next in %d", (int)std::ceil(left)));
        float tw = 176, th = 99;
        draw_thumb(n, x, y + 26, tw, th, 8, rgba(0, 0, 0));
        float tx = x + tw + 14;
        float ty = y + 26 + g_gfx.paragraph(tx, y + 26, w - tw - 14, col::text, 0.8f, n.title, 2, 1.0f, true) + 4;
        g_gfx.text(tx, ty, col::text2, 0.62f, g_gfx.ellipsize(n.channel, w - tw - 14, 0.62f));
        float by = y + 26 + th + 14;
        float cw = draw_pill(x, by, "Cancel", PillStyle::Secondary, countdownSel_ == 0, false, Icon::Play, 34);
        cdCancel_ = Rect{x, by, cw, 34};
        float pw = draw_pill(x + cw + 12, by, "Play now", PillStyle::Primary, countdownSel_ == 1, false, Icon::Play, 34);
        cdPlay_ = Rect{x + cw + 12, by, pw, 34};
    }

    static std::string C() { return g_app->confirmGlyph(); }

    // ------------------------------------------------------------ state

    Item item_;
    std::vector<Item> queue_;
    int qi_ = -1;

    PlayerInfo info_;
    bool infoLoaded_ = false;
    WatchInfo watch_;
    bool watchLoaded_ = false;
    std::string playError_;
    Format vfmt_, afmt_;
    bool haveStreams_ = false;
    std::unique_ptr<Player> player_;
    double resume_ = 0;
    bool resumeOnShow_ = false;
    bool ended_ = false;

    Captions caps_;
    bool capsOn_ = false;
    int capTrack_ = -1;
    int capGen_ = 0;

    std::vector<PanelTab> tabs_;
    int tab_ = 0;
    Focus focus_ = F_PLAYER;
    bool full_ = false;
    std::vector<Action> actions_;
    int actionSel_ = 0;
    Rect descBox_;
    Feed queueFeed_, related_;
    CommentsPanel comments_;
    bool commentsRequested_ = false;
    ListView chapterList_;
    bool loop_ = false;
    bool pendingTap_ = false;
    int pendingSide_ = 0, flashSide_ = 0;
    double pendingTapAt_ = 0, flashAt_ = -10;
    struct {
        std::vector<std::string> lines;
        std::vector<bool> bold;
        ListView list;
    } desc_;

    double lastInteract_ = 0, lastRecord_ = 0, lastSave_ = 0;
    bool seekPending_ = false;
    double seekTarget_ = 0, seekAt_ = 0;
    bool countdown_ = false;
    int countdownSel_ = 1;
    double nextAt_ = 0;
    Rect cdCancel_, cdPlay_;
};

std::unique_ptr<Screen> make_watch_screen(const Item& it, const std::vector<Item>& queue, int queueIndex) {
    return std::make_unique<WatchScreen>(it, queue, queueIndex);
}

}  // namespace vt
