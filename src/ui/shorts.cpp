// Shorts: a vertical player in the middle of the page with an action column beside it. Up and
// Down (or a swipe) move through the feed; each Short loops until you move on.
#include <algorithm>
#include <cmath>
#include <map>

#include "../core/parse.h"
#include "../core/util.h"
#include "../vita/player.h"
#include "app.h"
#include "comments.h"

namespace vt {

namespace {

constexpr float kPH = 472;   // player height
constexpr float kPW = 266;   // 9:16
constexpr float kPY = CONTENT.y + 10;
constexpr float kBtnR = 22;  // action button radius
constexpr float kBtnStep = 76;

enum ActionId { A_LIKE, A_DISLIKE, A_COMMENTS, A_SHARE, A_MORE, A_COUNT };

}  // namespace

class ShortsScreen : public Screen {
public:
    ShortsScreen(const std::vector<Item>& list, int index) {
        for (const Item& it : list) {
            Short s;
            s.item = it;
            s.item.shorts = true;
            shorts_.push_back(s);
        }
        idx_ = std::max(0, std::min(index, (int)shorts_.size() - 1));
        if (shorts_.empty()) loadSeed();
        else show(idx_);
    }
    ~ShortsScreen() override { stopPlayer(); }
    bool playsVideo() const override { return true; }

    void onHide() override {
        if (player_) resume_ = player_->position();
        stopPlayer();
        hidden_ = true;
    }
    void onShow() override {
        if (hidden_) {
            hidden_ = false;
            startPlayer(resume_);
        }
    }

    // Picking the Shorts tab again: a fresh set of Shorts after the current one.
    void refresh() override {
        if (seedLoading_) return;
        jumpTo_ = (int)shorts_.size();
        loadSeed();
    }

    void update(Input& in) override {
        double now = monotonic_seconds();
        if (player_) {
            Player::State st = player_->state();
            if (st == Player::State::Playing || st == Player::State::Buffering) g_app->keepAwake();
            if (st == Player::State::Ended) {  // Shorts loop: rewind, keeping the decoders
                record(cur().info.lengthSeconds);
                player_->seek(0);
                player_->setPaused(false);
            }
            if (st == Player::State::Playing && now - lastRecord_ > 5) {
                lastRecord_ = now;
                record(player_->position());
            }
        }
        float px = playerX();
        handleTouch(in, px);
        if (in.pressed(BTN_START)) togglePause();
        if (in.pressed(BTN_R)) move(1);
        if (in.pressed(BTN_L)) move(-1);
        if (in.pressed(BTN_SQUARE)) {
            in.consume(BTN_SQUARE);
            moreMenu();
            return;
        }
        if (focus_ == F_PLAYER) {
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                togglePause();
            }
            if (in.repeat(BTN_DOWN)) {
                in.consume(BTN_DOWN);
                move(1);
            }
            if (in.repeat(BTN_UP) && idx_ > 0) {
                in.consume(BTN_UP);
                move(-1);
            }
            if (in.pressed(BTN_RIGHT)) {
                in.consume(BTN_RIGHT);
                focus_ = F_ACTIONS;
            }
        } else if (focus_ == F_ACTIONS) {
            if (in.repeat(BTN_DOWN) && actionSel_ + 1 < A_COUNT) actionSel_++;
            if (in.repeat(BTN_UP) && actionSel_ > 0) actionSel_--;
            in.consume(BTN_UP | BTN_DOWN);
            if (in.pressed(BTN_LEFT)) {
                in.consume(BTN_LEFT);
                focus_ = F_PLAYER;
            }
            if (in.pressed(BTN_RIGHT) && commentsOpen_) {
                in.consume(BTN_RIGHT);
                focus_ = F_COMMENTS;
            }
            if (in.pressed(BTN_CONFIRM)) {
                in.consume(BTN_CONFIRM);
                runAction(actionSel_);
            }
        } else if (focus_ == F_COMMENTS) {
            bool hdr = comments_.headerFocused();
            comments_.update(in, commentsRect(), true);
            if (in.pressed(BTN_LEFT)) {
                in.consume(BTN_LEFT);
                focus_ = F_ACTIONS;
            }
            if (in.pressed(BTN_CANCEL)) {
                in.consume(BTN_CANCEL);
                closeComments();
            }
            if (hdr && in.pressed(BTN_UP)) in.consume(BTN_UP);  // stay in the panel
        }
        if (focus_ != F_COMMENTS && commentsOpen_) comments_.update(in, commentsRect(), false);
        if (commentsOpen_ && in.pressed(BTN_CANCEL)) {
            in.consume(BTN_CANCEL);
            closeComments();
        }
        // Keep the feed going.
        if (!seedLoading_ && idx_ + 3 >= (int)shorts_.size()) {
            if (!sequence_.empty()) loadSequence();
            else if (!seeded_) loadSeed();
        }
    }

    void draw() override {
        float target = commentsOpen_ ? CONTENT.x + 40 : CONTENT.x + (CONTENT.w - kPW) / 2 - 40;
        px_ = px_ < 0 ? target : px_ + (target - px_) * 0.25f;
        if (std::fabs(target - px_) < 0.5f) px_ = target;
        float x = px_, y = kPY;
        bool act = active();
        g_gfx.roundRect(x, y, kPW, kPH, 12, rgba(0, 0, 0));
        if (shorts_.empty()) {
            if (seedLoading_) g_gfx.spinner(x + kPW / 2, y + kPH / 2, 20, col::text);
            else draw_empty_state(Rect{x, y, kPW, kPH}, Icon::Shorts, "No Shorts right now", seedError_, "");
            g_gfx.cornerMask(x, y, kPW, kPH, 12, col::bg);
            return;
        }
        Short& s = cur();
        if (player_ && player_->state() != Player::State::Loading) {
            player_->draw(x, y, kPW, kPH);
        } else {
            std::string url = s.item.thumbUrl();
            if (!url.empty())
                if (vita2d_texture* t = g_app->thumbs.get(url)) g_gfx.imageCover(t, x, y, kPW, kPH);
        }
        Player::State st = player_ ? player_->state() : Player::State::Loading;
        std::string err = !s.error.empty() ? s.error : (st == Player::State::Error && player_ ? player_->error() : "");
        // Bottom scrim with the channel, Subscribe and the title.
        g_gfx.vgradient(x, y + kPH - 170, kPW, 170, rgba(0, 0, 0, 0), rgba(0, 0, 0, 190));
        float ty = y + kPH - 18;
        std::string title = s.item.title;
        const auto& lines = g_gfx.wrap(title, kPW - 28, 0.7f, 2);
        ty -= lines.size() * g_gfx.lineHeight(0.7f);
        for (size_t i = 0; i < lines.size(); i++) g_gfx.text(x + 14, ty + i * g_gfx.lineHeight(0.7f), col::text, 0.7f, lines[i]);
        float cy = ty - 26;
        std::string ch = s.item.channel.empty() ? "" : (s.item.channel[0] == '@' ? s.item.channel : "@" + nospace(s.item.channel));
        draw_avatar(avatar(), s.item.channel, x + 28, cy + 2, 14, rgba(0, 0, 0));
        float nx = x + 48;
        if (!ch.empty()) {
            std::string n = g_gfx.ellipsize(ch, kPW - 150, 0.68f, W_MEDIUM);
            g_gfx.text(nx, cy - g_gfx.lineHeight(0.68f) / 2 + 2, col::text, 0.68f, n, W_MEDIUM);
            nx += g_gfx.width(n, 0.68f, W_MEDIUM) + 10;
        }
        subBtn_ = Rect{};
        if (!s.item.channelId.empty()) {
            bool sub = g_app->store.isSubscribed(s.item.channelId);
            std::string label = sub ? "Subscribed" : "Subscribe";
            float w = pill_width(label, false, 28);
            subBtn_ = Rect{nx, cy - 12, w, 28};
            draw_pill(nx, cy - 12, label, sub ? PillStyle::Secondary : PillStyle::Primary, false, false, Icon::Play, 28);
        }
        // Thin progress bar along the bottom.
        if (player_) {
            double d = player_->duration();
            if (d > 0) {
                float f = (float)std::min(1.0, player_->position() / d);
                g_gfx.rect(x, y + kPH - 3, kPW, 3, rgba(255, 255, 255, 60));
                g_gfx.hgradient(x, y + kPH - 3, kPW * f, 3, col::red, col::redPink);
            }
        }
        bool busy = st == Player::State::Loading || st == Player::State::Buffering || !s.infoDone;
        if (!err.empty()) {
            g_gfx.rect(x, y, kPW, kPH, rgba(0, 0, 0, 200));
            draw_empty_state(Rect{x + 10, y, kPW - 20, kPH}, Icon::Info, "Can't play this Short", err);
        } else if (busy) {
            g_gfx.spinner(x + kPW / 2, y + kPH / 2, 20, col::text);
        } else if (player_ && player_->paused()) {
            g_gfx.circle(x + kPW / 2, y + kPH / 2, 32, rgba(0, 0, 0, 120));
            draw_icon(Icon::Play, x + kPW / 2 + 2, y + kPH / 2, 34, col::text, rgba(0, 0, 0, 120));
        }
        g_gfx.cornerMask(x, y, kPW, kPH, 12, col::bg);
        if (act && focus_ == F_PLAYER) g_gfx.roundFrame(x - 4, y - 4, kPW + 8, kPH + 8, 16, 2.5f, col::focus);

        drawActions(x + kPW + 14, act);
        drawNav(act);
        if (commentsOpen_) {
            Rect cr = commentsRect();
            g_gfx.roundRect(cr.x - 6, cr.y - 4, cr.w + 6, cr.h + 8, 12, rgba(33, 33, 33));
            g_gfx.clip(cr);
            comments_.draw(cr, act && focus_ == F_COMMENTS);
            g_gfx.unclip();
        }
    }

private:
    enum Focus { F_PLAYER, F_ACTIONS, F_COMMENTS };
    struct Short {
        Item item;
        bool infoReq = false, infoDone = false, nextReq = false, nextDone = false;
        PlayerInfo info;
        WatchInfo watch;
        std::string error;
    };

    Short& cur() { return shorts_[idx_]; }
    float playerX() const { return px_ < 0 ? CONTENT.x + (CONTENT.w - kPW) / 2 - 40 : px_; }
    Rect commentsRect() const {
        float x = playerX() + kPW + 14 + 2 * kBtnR + 24;
        return Rect{x, kPY + 4, SCREEN_W - 12 - x, kPH - 8};
    }
    std::string avatar() {
        Short& s = cur();
        if (!s.watch.channelAvatar.empty()) return s.watch.channelAvatar;
        return s.item.avatar;
    }
    static std::string nospace(const std::string& s) {
        std::string o;
        for (char c : s)
            if (c != ' ') o += c;
        return o;
    }

    // ------------------------------------------------------------ feed

    void loadSeed() {
        seeded_ = true;
        seedLoading_ = true;
        async<ReelSeed>([] { return g_app->yt.reelSeed(); }, [this](ReelSeed& r) {
            seedLoading_ = false;
            if (!r.error.empty() && r.videoId.empty()) {
                seedError_ = r.error;
                return;
            }
            bool first = shorts_.empty();
            if (!r.videoId.empty() && !has(r.videoId)) {
                Short s;
                s.item.id = r.videoId;
                s.item.shorts = true;
                shorts_.push_back(s);
            }
            sequence_ = r.sequence;
            if (first && !shorts_.empty()) show(0);
            else jumpToNew();
            if (!sequence_.empty()) loadSequence();
        });
    }

    void loadSequence() {
        if (seedLoading_ || sequence_.empty()) return;
        seedLoading_ = true;
        std::string seq = sequence_;
        sequence_.clear();
        async<Page>([seq] { return g_app->yt.reelSequence(seq); }, [this](Page& p) {
            seedLoading_ = false;
            bool first = shorts_.empty();
            for (auto& it : p.items) {
                if (has(it.id)) continue;
                Short s;
                s.item = it;
                shorts_.push_back(s);
            }
            sequence_ = p.continuation;
            if (first && !shorts_.empty()) show(0);
            else jumpToNew();
            prefetch(idx_ + 1);
        });
    }

    void jumpToNew() {
        if (jumpTo_ < 0 || jumpTo_ >= (int)shorts_.size()) return;
        int j = jumpTo_;
        jumpTo_ = -1;
        show(j);
    }

    bool has(const std::string& id) const {
        for (auto& s : shorts_)
            if (s.item.id == id) return true;
        return false;
    }

    void requestInfo(int i) {
        if (i < 0 || i >= (int)shorts_.size()) return;
        Short& s = shorts_[i];
        if (!s.infoReq) {
            s.infoReq = true;
            std::string id = s.item.id;
            async<PlayerInfo>([id] { return g_app->yt.player(id); }, [this, id](PlayerInfo& p) {
                for (int k = 0; k < (int)shorts_.size(); k++) {
                    Short& t = shorts_[k];
                    if (t.item.id != id) continue;
                    t.info = p;
                    t.infoDone = true;
                    if (!p.title.empty()) t.item.title = p.title;
                    if (!p.author.empty()) t.item.channel = p.author;
                    if (!p.channelId.empty()) t.item.channelId = p.channelId;
                    if (p.viewCount >= 0) t.item.views = format_views(p.viewCount);
                    if (p.lengthSeconds > 0) t.item.duration = format_duration(p.lengthSeconds);
                    if (!p.ok()) t.error = p.reason.empty() ? "This Short is unavailable." : p.reason;
                    if (k == idx_ && !hidden_) startPlayer(0);
                }
            });
        }
        if (!s.nextReq) {
            s.nextReq = true;
            std::string id = s.item.id;
            async<WatchInfo>([id] { return g_app->yt.next(id); }, [this, id](WatchInfo& w) {
                for (int k = 0; k < (int)shorts_.size(); k++) {
                    Short& t = shorts_[k];
                    if (t.item.id != id) continue;
                    t.watch = w;
                    t.nextDone = true;
                    if (t.item.channelId.empty()) t.item.channelId = w.channelId;
                    if (t.item.channel.empty()) t.item.channel = w.channelName;
                    t.item.verified = w.verified;
                    if (k == idx_ && commentsOpen_ && !comments_.started())
                        comments_.start(w.commentsContinuation, w.commentSorts, w.commentCount);
                }
            });
        }
    }

    void prefetch(int i) {
        if (i >= 0 && i < (int)shorts_.size()) requestInfo(i);
    }

    void show(int i) {
        stopPlayer();
        idx_ = i;
        Short& s = cur();
        if (s.infoDone) startPlayer(0);
        else requestInfo(i);
        prefetch(i + 1);
        if (commentsOpen_) {
            comments_.clear();
            if (s.nextDone) comments_.start(s.watch.commentsContinuation, s.watch.commentSorts, s.watch.commentCount);
        }
    }

    void move(int d) {
        int n = idx_ + d;
        if (n < 0 || n >= (int)shorts_.size()) return;
        show(n);
    }

    // ------------------------------------------------------------ playback

    void startPlayer(double at) {
        stopPlayer();
        if (shorts_.empty()) return;
        Short& s = cur();
        if (!s.infoDone || !s.info.ok()) return;
        Format v, a;
        InnerTube& yt = g_app->yt;
        if (!pick_streams(s.info, g_app->store.settings.maxHeight, yt.videoItags(), yt.audioItags(), v, a)) {
            s.error = "YouTube didn't offer a stream this system can decode for this Short.";
            return;
        }
        player_ = std::make_unique<Player>();
        if (!player_->start(v, a, yt.playerUserAgent(), s.info.lengthSeconds, at)) {
            s.error = player_->error().empty() ? "Couldn't start the player." : player_->error();
            player_.reset();
        }
    }

    void stopPlayer() {
        if (!player_) return;
        record(player_->position());
        g_app->store.saveHistory();
        player_->stop();
        player_.reset();
    }

    void record(double pos) {
        if (shorts_.empty()) return;
        Short& s = cur();
        if (!s.infoDone || !s.info.ok()) return;
        Item it = s.item;
        if (it.avatar.empty()) it.avatar = s.watch.channelAvatar;
        g_app->store.recordWatch(it, (int)pos, s.info.lengthSeconds);
    }

    void togglePause() {
        if (!player_) {
            startPlayer(0);
            return;
        }
        player_->setPaused(!player_->paused());
    }

    // ------------------------------------------------------------ actions

    struct ActionSpec {
        Icon icon;
        std::string label;
        bool on;
    };
    ActionSpec spec(int a) {
        Short& s = cur();
        int r = g_app->store.rating(s.item.id);
        switch (a) {
            case A_LIKE: return {r == 1 ? Icon::ThumbUpFilled : Icon::ThumbUp, s.watch.likes.empty() ? "Like" : s.watch.likes, r == 1};
            case A_DISLIKE: return {r == -1 ? Icon::ThumbDownFilled : Icon::ThumbDown, "Dislike", r == -1};
            case A_COMMENTS:
                return {Icon::Comment, s.watch.commentCount.empty() ? "Comments" : s.watch.commentCount, commentsOpen_};
            case A_SHARE: return {Icon::Share, "Share", false};
            default: return {Icon::More, "", false};
        }
    }

    float actionY(int a) const { return kPY + kPH - (A_COUNT - a) * kBtnStep + 8; }

    void drawActions(float x, bool act) {
        if (shorts_.empty()) return;
        actionX_ = x;
        for (int a = 0; a < A_COUNT; a++) {
            ActionSpec sp = spec(a);
            float cx = x + kBtnR, cy = actionY(a) + kBtnR;
            bool foc = act && focus_ == F_ACTIONS && actionSel_ == a;
            uint32_t fill = sp.on && a == A_COMMENTS ? col::text : foc ? col::raisedHi : col::raised;
            g_gfx.circle(cx, cy, kBtnR, fill);
            draw_icon(sp.icon, cx, cy, 24, sp.on && a == A_COMMENTS ? col::onLight : col::text, fill);
            if (foc) g_gfx.ring(cx, cy, kBtnR + 4, 2.5f, col::focus);
            if (!sp.label.empty())
                g_gfx.textCentered(cx, cy + kBtnR + 4, col::text, 0.58f, g_gfx.ellipsize(sp.label, 70, 0.58f), W_MEDIUM);
        }
    }

    // Up / down arrows at the right edge.
    void drawNav(bool act) {
        (void)act;
        if (commentsOpen_ || shorts_.empty()) return;
        float cx = CONTENT.x + CONTENT.w - 44;
        float cy = kPY + kPH / 2;
        bool up = idx_ > 0, down = idx_ + 1 < (int)shorts_.size();
        // Up: the down chevron mirrored, drawn as two strokes.
        g_gfx.circle(cx, cy - 34, 26, up ? col::raised : rgba(39, 39, 39, 120));
        uint32_t uc = up ? col::text : col::text3;
        g_gfx.line(cx - 8, cy - 30, cx, cy - 38, 2.5f, uc);
        g_gfx.line(cx, cy - 38, cx + 8, cy - 30, 2.5f, uc);
        g_gfx.circle(cx, cy - 38, 1.25f, uc);
        g_gfx.circle(cx, cy + 34, 26, down ? col::raised : rgba(39, 39, 39, 120));
        draw_icon(Icon::ChevronDown, cx, cy + 34, 22, down ? col::text : col::text3, col::raised);
        navUp_ = Rect{cx - 26, cy - 60, 52, 52};
        navDown_ = Rect{cx - 26, cy + 8, 52, 52};
    }

    void runAction(int a) {
        if (shorts_.empty()) return;
        Short& s = cur();
        switch (a) {
            case A_LIKE: g_app->rateVideo(s.item, g_app->store.rating(s.item.id) == 1 ? 0 : 1); break;
            case A_DISLIKE: g_app->rateVideo(s.item, g_app->store.rating(s.item.id) == -1 ? 0 : -1); break;
            case A_COMMENTS:
                if (commentsOpen_) closeComments();
                else openComments();
                break;
            case A_SHARE: g_app->shareItem(s.item); break;
            default: moreMenu(); break;
        }
    }

    void openComments() {
        commentsOpen_ = true;
        focus_ = F_COMMENTS;
        Short& s = cur();
        comments_.clear();
        if (s.nextDone) comments_.start(s.watch.commentsContinuation, s.watch.commentSorts, s.watch.commentCount);
    }

    void closeComments() {
        commentsOpen_ = false;
        comments_.clear();
        if (focus_ == F_COMMENTS) focus_ = F_ACTIONS;
    }

    void moreMenu() {
        if (shorts_.empty()) return;
        Item it = cur().item;
        std::vector<MenuAction> acts;
        acts.push_back({"Save to playlist", Icon::Save, [it] { g_app->saveToPlaylist(it); }});
        acts.push_back({"Open as a regular video", Icon::Play, [it] {
                            Item v = it;
                            v.shorts = false;
                            g_app->openVideo(v);
                        }});
        if (!it.channelId.empty()) {
            bool sub = g_app->store.isSubscribed(it.channelId);
            std::string av = cur().watch.channelAvatar;
            acts.push_back({sub ? "Unsubscribe" : "Subscribe", Icon::Subscriptions,
                            [it, av] { g_app->toggleSubscription(it.channelId, it.channel, av); }});
            acts.push_back({"Go to channel", Icon::Account, [it] { g_app->openChannel(it.channelId, it.channel); }});
        }
        g_app->showActions(it.title, acts);
    }

    // ------------------------------------------------------------ touch

    void handleTouch(Input& in, float px) {
        Rect pr{px, kPY, kPW, kPH};
        // Vertical swipe on the player moves through the feed.
        if (in.touchEnded && pr.contains(in.startX, in.startY) && std::fabs(in.ty - in.startY) > 70 && !in.tapped) {
            move(in.ty < in.startY ? 1 : -1);
            return;
        }
        if (!in.tapped) return;
        if (subBtn_.w > 0 && in.tapIn(subBtn_.x - 4, subBtn_.y - 4, subBtn_.w + 8, subBtn_.h + 8)) {
            in.consumeTap();
            Short& s = cur();
            g_app->toggleSubscription(s.item.channelId, s.item.channel, s.watch.channelAvatar);
            return;
        }
        if (pr.contains(in.tapX, in.tapY)) {
            in.consumeTap();
            focus_ = F_PLAYER;
            togglePause();
            return;
        }
        for (int a = 0; a < A_COUNT && !shorts_.empty(); a++) {
            float cx = actionX_ + kBtnR, cy = actionY(a) + kBtnR;
            if (std::fabs(in.tapX - cx) < kBtnR + 6 && std::fabs(in.tapY - cy) < kBtnR + 6) {
                in.consumeTap();
                focus_ = F_ACTIONS;
                actionSel_ = a;
                runAction(a);
                return;
            }
        }
        if (navUp_.contains(in.tapX, in.tapY)) {
            in.consumeTap();
            move(-1);
        } else if (navDown_.contains(in.tapX, in.tapY)) {
            in.consumeTap();
            move(1);
        }
    }

    std::vector<Short> shorts_;
    int idx_ = 0, jumpTo_ = -1;
    std::string sequence_, seedError_;
    bool seeded_ = false, seedLoading_ = false, hidden_ = false;
    std::unique_ptr<Player> player_;
    double resume_ = 0, lastRecord_ = 0;
    Focus focus_ = F_PLAYER;
    int actionSel_ = 0;
    bool commentsOpen_ = false;
    CommentsPanel comments_;
    float px_ = -1, actionX_ = 0;
    Rect subBtn_, navUp_, navDown_;
};

std::unique_ptr<Screen> make_shorts_screen(const std::vector<Item>& list, int index) {
    return std::make_unique<ShortsScreen>(list, index);
}

}  // namespace vt
