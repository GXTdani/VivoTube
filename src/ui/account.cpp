// Sign in with Google (device code flow) and the update dialog.
#include <algorithm>
#include <atomic>
#include <cmath>

#include "../core/http.h"
#include "../core/util.h"
#include "../vita/updater.h"
#include "app.h"

namespace vt {

namespace {

// A centered card with a title; returns the content top.
float draw_card(float w, float h, const std::string& title, Rect* out = nullptr) {
    float x = CONTENT.x + (CONTENT.w - w) / 2, y = CONTENT.y + std::max(8.0f, (CONTENT.h - h) / 2);
    g_gfx.roundRect(x, y, w, h, 16, rgba(33, 33, 33));
    g_gfx.text(x + 28, y + 22, col::text, 1.1f, title, W_BOLD);
    if (out) *out = Rect{x, y, w, h};
    return y + 62;
}

// Horizontal row of pill buttons; focus index sel. Returns their rects.
std::vector<Rect> draw_buttons(float x, float y, const std::vector<std::string>& labels, int sel, bool active,
                               int primary = 0) {
    std::vector<Rect> rs;
    for (size_t i = 0; i < labels.size(); i++) {
        float w = draw_pill(x, y, labels[i], (int)i == primary ? PillStyle::Primary : PillStyle::Secondary,
                            active && (int)i == sel);
        rs.push_back(Rect{x, y, w, 36});
        x += w + 12;
    }
    return rs;
}

int buttons_input(Input& in, const std::vector<Rect>& rs, int& sel) {
    int n = (int)rs.size();
    if (in.repeat(BTN_RIGHT) && sel + 1 < n) {
        in.consume(BTN_RIGHT);
        sel++;
    }
    if (in.repeat(BTN_LEFT) && sel > 0) {
        in.consume(BTN_LEFT);
        sel--;
    }
    if (in.pressed(BTN_CONFIRM)) {
        in.consume(BTN_CONFIRM);
        return sel;
    }
    for (int i = 0; i < n; i++)
        if (in.tapIn(rs[i].x - 4, rs[i].y - 4, rs[i].w + 8, rs[i].h + 8)) {
            in.consumeTap();
            sel = i;
            return i;
        }
    return -1;
}

}  // namespace

// ================================================================ Sign in

class SignInScreen : public Screen {
public:
    SignInScreen() { begin(); }

    void update(Input& in) override {
        int b = buttons_input(in, btns_, sel_);
        if (b >= 0) press(b);
        if (state_ == S_WAITING && monotonic_seconds() >= nextPoll_ && !polling_) poll();
        if (state_ == S_WAITING && monotonic_seconds() > expires_) {
            state_ = S_ERROR;
            error_ = "The code expired. Get a new one to try again.";
        }
    }

    void draw() override {
        bool act = active();
        float w = 640, h = state_ == S_WAITING ? 460 : 400;
        Rect card;
        float y = draw_card(w, h, "Sign in with Google", &card);
        float x = card.x + 28, tw = w - 56;
        std::vector<std::string> labels;
        switch (state_) {
            case S_SETUP: {
                y += g_gfx.paragraph(x, y, tw, col::text2, 0.68f,
                                     "VivoTube signs in with your own Google Cloud OAuth client, so nothing goes "
                                     "through anyone else's servers. One-time setup:",
                                     3) + 8;
                const char* steps[] = {
                    "1. In console.cloud.google.com, create a project and enable YouTube Data API v3.",
                    "2. Set up the OAuth consent screen (External) and add your Google account as a test user.",
                    "3. Create an OAuth client ID of type \"TVs and Limited Input devices\" and download its JSON.",
                    "4. Copy the file to ux0:data/VivoTube/google_client.json (VitaShell FTP or USB)."};
                for (const char* s : steps) y += g_gfx.paragraph(x, y, tw, col::text, 0.64f, s, 2) + 6;
                y += 4;
                g_gfx.paragraph(x, y, tw, col::text3, 0.58f,
                                "The README on GitHub explains each step. While the app is in Testing, "
                                "Google asks you to sign in again every 7 days.",
                                2);
                labels = {"Check again", "Cancel"};
                break;
            }
            case S_REQUESTING:
                g_gfx.spinner(card.x + w / 2, card.y + h / 2, 20, col::text2);
                g_gfx.textCentered(card.x + w / 2, card.y + h / 2 + 34, col::text2, 0.68f, "Getting a sign-in code...");
                labels = {"Cancel"};
                break;
            case S_WAITING: {
                g_gfx.text(x, y, col::text2, 0.72f, "On your phone or computer, go to");
                y += 26;
                g_gfx.text(x, y, col::blue, 1.15f, url_, W_MEDIUM);
                y += 40;
                g_gfx.text(x, y, col::text2, 0.72f, "and enter this code:");
                y += 30;
                float cw = g_gfx.width(code_, 1.9f, W_BOLD) + 48;
                g_gfx.roundRect(x, y, cw, 64, 12, col::raised);
                g_gfx.text(x + 24, y + (64 - g_gfx.lineHeight(1.9f)) / 2, col::text, 1.9f, code_, W_BOLD);
                y += 82;
                g_gfx.spinner(x + 10, y + 10, 8, col::text2);
                int left = std::max(0, (int)(expires_ - monotonic_seconds()));
                g_gfx.text(x + 28, y, col::text2, 0.64f,
                           str_printf("Waiting for you to approve VivoTube " G_DOT " code expires in %d:%02d", left / 60,
                                      left % 60));
                y += 40;
                y += g_gfx.paragraph(x, y, tw, col::text2, 0.62f,
                                     "If Google warns that it hasn't verified this app, choose Advanced, then "
                                     "Go to \xE2\x80\xA6 (unsafe).",
                                     2) + 10;
                g_gfx.paragraph(x, y, tw, col::text3, 0.58f,
                                "By signing in, you agree to the YouTube Terms of Service and VivoTube's privacy "
                                "policy: hamburger101.github.io/VivoTube/privacy.html",
                                2);
                labels = {"Get a new code", "Cancel"};
                break;
            }
            case S_FINISHING:
                g_gfx.spinner(card.x + w / 2, card.y + h / 2, 20, col::text2);
                g_gfx.textCentered(card.x + w / 2, card.y + h / 2 + 34, col::text2, 0.68f, "Signing in...");
                labels = {};
                break;
            case S_ERROR:
                y += g_gfx.paragraph(x, y, tw, col::text, 0.72f, error_, 6) + 10;
                labels = {"Try again", "Cancel"};
                break;
        }
        btns_ = draw_buttons(x, card.y + h - 60, labels, sel_, act);
        if (sel_ >= (int)labels.size()) sel_ = 0;
    }

private:
    enum State { S_SETUP, S_REQUESTING, S_WAITING, S_FINISHING, S_ERROR };

    void begin() {
        sel_ = 0;
        if (!g_app->account.configured()) {
            state_ = S_SETUP;
            return;
        }
        state_ = S_REQUESTING;
        uint32_t gen = ++gen_;
        async<DeviceCode>([] { return g_app->account.requestCode(); }, [this, gen](DeviceCode& c) {
            if (gen != gen_) return;
            if (!c.error.empty()) {
                state_ = S_ERROR;
                error_ = "Google refused the request: " + c.error +
                         (g_app->account.ownClient()
                              ? "\n\nCheck that google_client.json is a \"TVs and Limited Input devices\" client."
                              : "\n\nVivoTube's built-in sign-in isn't available right now. You can use your own "
                                "Google OAuth client instead: see \"Signing in with Google\" in the README on GitHub.");
                return;
            }
            device_ = c.deviceCode;
            code_ = c.userCode;
            url_ = c.verifyUrl;
            if (starts_with(url_, "https://")) url_ = url_.substr(8);
            if (starts_with(url_, "www.")) url_ = url_.substr(4);
            interval_ = std::max(5, c.interval);
            expires_ = monotonic_seconds() + c.expiresIn;
            nextPoll_ = monotonic_seconds() + interval_;
            state_ = S_WAITING;
        });
    }

    void poll() {
        polling_ = true;
        std::string dc = device_;
        uint32_t gen = gen_;
        async<TokenResult>([dc] { return g_app->account.pollToken(dc); }, [this, gen](TokenResult& t) {
            polling_ = false;
            if (gen != gen_ || state_ != S_WAITING) return;
            if (t.pending()) {
                if (t.error == "slow_down") interval_ += 5;
                nextPoll_ = monotonic_seconds() + interval_;
                return;
            }
            if (!t.error.empty()) {
                state_ = S_ERROR;
                error_ = t.error == "access_denied" ? "Sign-in was cancelled on the other device."
                         : t.error == "expired_token" ? "The code expired. Get a new one to try again."
                                                      : "Sign-in failed: " + t.error;
                return;
            }
            finish(t);
        });
    }

    void finish(const TokenResult& t) {
        state_ = S_FINISHING;
        struct R {
            bool ok = false;
            std::string err;
        };
        TokenResult tok = t;
        async<R>([tok] {
            R r;
            r.ok = g_app->account.completeSignIn(tok, &r.err);
            return r;
        }, [this](R& r) {
            if (!r.ok) {
                state_ = S_ERROR;
                error_ = "Signed in, but YouTube didn't return your channel: " + r.err;
                return;
            }
            g_app->accountGen++;
            g_app->toast("Signed in as " + g_app->account.profile().name);
            g_app->syncSubscriptions(true);
            if (g_app->top() == this) g_app->pop();
        });
    }

    void press(int b) {
        switch (state_) {
            case S_SETUP:
                if (b == 0) {
                    g_app->account.init(g_app->dataDir);
                    if (g_app->account.configured()) begin();
                    else g_app->toast("google_client.json wasn't found in " + g_app->dataDir);
                } else {
                    g_app->pop();
                }
                break;
            case S_WAITING:
            case S_ERROR:
                if (b == 0) begin();
                else g_app->pop();
                break;
            case S_REQUESTING:
                g_app->pop();
                break;
            default: break;
        }
    }

    State state_ = S_SETUP;
    std::string device_, code_, url_ = "google.com/device", error_;
    int interval_ = 5;
    double expires_ = 0, nextPoll_ = 0;
    bool polling_ = false;
    uint32_t gen_ = 0;
    std::vector<Rect> btns_;
    int sel_ = 0;
};

// ================================================================ Update

class UpdateScreen : public Screen {
public:
    explicit UpdateScreen(const Release& r) : rel_(r) {
        notes_.rowH = g_gfx.lineHeight(0.64f);
        notesLines_ = g_gfx.wrap(rel_.notes.empty() ? "No release notes." : rel_.notes, 560, 0.64f, 400);
        notes_.count = (int)notesLines_.size();
    }

    void update(Input& in) override {
        if (state_ == S_DOWNLOADING && prog_->installing) state_ = S_INSTALLING;
        if (state_ == S_OFFER || state_ == S_ERROR || state_ == S_DONE) {
            int b = buttons_input(in, btns_, sel_);
            if (b >= 0) press(b);
            // Up / Down scroll the notes.
            if (in.repeat(BTN_DOWN)) notes_.scrollBy(notesRect_, 60);
            if (in.repeat(BTN_UP)) notes_.scrollBy(notesRect_, -60);
            in.consume(BTN_UP | BTN_DOWN | BTN_LEFT);
            notes_.focused = false;
            notes_.update(in, notesRect_);
        } else {
            in.consume(0xFFFFFFFF);  // no way out while files are being replaced
            in.consumeTap();
        }
        if (state_ == S_DONE && monotonic_seconds() > restartAt_ && !restartTried_) {
            restartTried_ = true;
            if (!relaunch_app()) {
                g_app->toast("Close VivoTube and open it again to finish updating");
            }
        }
    }

    void draw() override {
        bool act = active();
        float w = 660, h = 440;
        Rect card;
        float y = draw_card(w, h, "Update available", &card);
        float x = card.x + 28, tw = w - 56;
        g_gfx.text(x, y - 18, col::text2, 0.68f,
                   "VivoTube " + rel_.version + " is out " G_DOT " you have " VT_VERSION +
                       (rel_.vpkSize > 0 ? str_printf(" " G_DOT " %.1f MB", rel_.vpkSize / 1048576.0) : std::string()));
        y += 12;
        std::vector<std::string> labels;
        if (state_ == S_OFFER || state_ == S_ERROR) {
            if (!rel_.name.empty()) {
                g_gfx.text(x, y, col::text, 0.8f, g_gfx.ellipsize(rel_.name, tw, 0.8f, W_MEDIUM), W_MEDIUM);
                y += 28;
            }
            float nh = card.y + h - 80 - y;
            if (state_ == S_ERROR) {
                y += g_gfx.paragraph(x, y, tw, rgba(255, 120, 120), 0.66f, error_, 4) + 8;
                nh = card.y + h - 80 - y;
            }
            notesRect_ = Rect{x, y, tw, nh};
            notes_.draw(notesRect_, [&](int i, const Rect& r, bool) {
                g_gfx.text(r.x, r.y, col::text, 0.64f, notesLines_[i]);
            });
            labels = {state_ == S_ERROR ? "Try again" : "Update now", "Later", "Skip this version"};
        } else if (state_ == S_DOWNLOADING || state_ == S_INSTALLING) {
            float frac = 0;
            std::string line;
            if (state_ == S_DOWNLOADING) {
                int64_t now = prog_->got.load(), all = std::max<int64_t>(prog_->size.load(), rel_.vpkSize);
                frac = all > 0 ? (float)now / all : 0;
                line = str_printf("Downloading... %.1f / %.1f MB", now / 1048576.0, all / 1048576.0);
            } else {
                int t = std::max(1, prog_->total.load());
                frac = (float)prog_->done.load() / t;
                line = "Installing... don't turn off the system";
            }
            float by = card.y + h / 2 - 10;
            g_gfx.text(x, by - 34, col::text, 0.72f, line);
            g_gfx.roundRect(x, by, tw, 8, 4, rgba(255, 255, 255, 40));
            g_gfx.roundRect(x, by, std::max(8.0f, tw * std::min(1.0f, frac)), 8, 4, col::red);
        } else if (state_ == S_DONE) {
            g_gfx.paragraph(x, card.y + h / 2 - 40, tw, col::text, 0.76f,
                            "Update installed. VivoTube is restarting...", 2);
            labels = {"Restart now"};
        }
        btns_ = draw_buttons(x, card.y + h - 60, labels, sel_, act);
        if (sel_ >= (int)labels.size()) sel_ = 0;
    }

private:
    enum State { S_OFFER, S_DOWNLOADING, S_INSTALLING, S_DONE, S_ERROR };

    void press(int b) {
        if (state_ == S_DONE) {
            relaunch_app();
            return;
        }
        if (b == 0) start();
        else if (b == 1) g_app->pop();
        else {
            g_app->store.settings.skipVersion = rel_.version;
            g_app->store.saveSettings();
            g_app->toast("You won't be reminded about " + rel_.version);
            g_app->pop();
        }
    }

    void start() {
        state_ = S_DOWNLOADING;
        prog_ = std::make_shared<Progress>();
        struct R {
            bool ok = false;
            std::string err, saved;
        };
        std::string url = rel_.vpkUrl, ver = rel_.version;
        std::shared_ptr<Progress> prog = prog_;
        // The worker reports through `prog`, which it shares, so closing the screen is safe.
        async<R>([url, ver, prog] {
            R r;
            HttpRequest req;
            req.url = url;
            req.userAgent = "VivoTube/" VT_VERSION;
            req.headers = {"Accept: application/octet-stream"};
            req.progressNow = &prog->got;
            req.progressTotal = &prog->size;
            req.maxBytes = 96u << 20;
            req.stallTimeout = 30;
            HttpResponse res = http_request(req);
            if (!res.ok()) {
                r.err = "Download failed (" + res.describe() + ").";
                return r;
            }
            prog->installing = true;
            if (install_update(res.body, kAppDir, &r.err, &prog->done, &prog->total)) {
                r.ok = true;
                return r;
            }
            // Keep the package so it can be installed by hand.
            r.saved = "ux0:data/VivoTube/VivoTube-" + ver + ".vpk";
            if (!write_file_atomic(r.saved, res.body)) r.saved.clear();
            r.err = "Couldn't install the update: " + r.err;
            return r;
        }, [this](R& r) {
            if (r.ok) {
                state_ = S_DONE;
                restartAt_ = monotonic_seconds() + 1.5;
                return;
            }
            state_ = S_ERROR;
            error_ = r.err;
            if (!r.saved.empty()) error_ += " The package was saved to " + r.saved + "; install it with VitaShell.";
        });
    }

    Release rel_;
    State state_ = S_OFFER;
    std::string error_;
    std::vector<std::string> notesLines_;
    ListView notes_;
    Rect notesRect_;
    std::vector<Rect> btns_;
    int sel_ = 0;
    struct Progress {
        std::atomic<int64_t> got{0}, size{0};
        std::atomic<int> done{0}, total{1};
        std::atomic<bool> installing{false};
    };
    std::shared_ptr<Progress> prog_ = std::make_shared<Progress>();
    double restartAt_ = 0;
    bool restartTried_ = false;
};

std::unique_ptr<Screen> make_signin_screen() { return std::make_unique<SignInScreen>(); }
std::unique_ptr<Screen> make_update_screen(const Release& r) { return std::make_unique<UpdateScreen>(r); }

}  // namespace vt
