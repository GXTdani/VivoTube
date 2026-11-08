#include "google.h"

#include <cctype>

#include "http.h"
#include "util.h"

#ifndef VT_GOOGLE_CLIENT_ID
#define VT_GOOGLE_CLIENT_ID ""
#endif
#ifndef VT_GOOGLE_CLIENT_SECRET
#define VT_GOOGLE_CLIENT_SECRET ""
#endif

namespace vt {

namespace {

const char* const kScope = "https://www.googleapis.com/auth/youtube";
const char* const kApi = "https://www.googleapis.com/youtube/v3/";

std::string best_thumb(const JVal* thumbs) {
    if (!thumbs) return std::string();
    for (const char* k : {"medium", "high", "default", "standard"}) {
        std::string u = jstr(*thumbs, (std::string(k) + ".url").c_str());
        if (!u.empty()) return u;
    }
    return std::string();
}

HttpResponse form_post(const std::string& url, const std::string& body) {
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.body = body;
    req.headers.push_back("Content-Type: application/x-www-form-urlencoded");
    req.connectTimeout = 10;
    return http_request(req);
}

}  // namespace

// ---------------------------------------------------------------- parsers

int iso8601_seconds(const std::string& d) {
    int total = 0, num = 0;
    bool time = false;
    for (char c : d) {
        if (isdigit((unsigned char)c)) {
            num = num * 10 + (c - '0');
            continue;
        }
        switch (c) {
            case 'T': time = true; break;
            case 'D': total += num * 86400; break;
            case 'H': total += num * 3600; break;
            case 'M': total += time ? num * 60 : 0; break;
            case 'S': total += num; break;
            default: break;
        }
        num = 0;
    }
    return total;
}

DeviceCode parse_device_code(const JVal& root) {
    DeviceCode c;
    c.error = jstr(jget(root, "error"));
    c.deviceCode = jstr(jget(root, "device_code"));
    c.userCode = jstr(jget(root, "user_code"));
    c.verifyUrl = jstr(jget(root, "verification_url"));
    if (c.verifyUrl.empty()) c.verifyUrl = jstr(jget(root, "verification_uri"));
    c.interval = (int)jint(jget(root, "interval"), 5);
    c.expiresIn = (int)jint(jget(root, "expires_in"), 1800);
    if (c.interval < 1) c.interval = 5;
    if (c.error.empty() && (c.deviceCode.empty() || c.userCode.empty())) c.error = "unexpected reply";
    std::string desc = jstr(jget(root, "error_description"));
    if (!desc.empty()) c.error += ": " + desc;
    return c;
}

TokenResult parse_token(const JVal& root) {
    TokenResult t;
    t.error = jstr(jget(root, "error"));
    t.access = jstr(jget(root, "access_token"));
    t.refresh = jstr(jget(root, "refresh_token"));
    t.expiresIn = (int)jint(jget(root, "expires_in"), 3600);
    if (t.error.empty() && t.access.empty()) t.error = "unexpected reply";
    return t;
}

bool parse_profile(const JVal& root, GoogleProfile& p) {
    const JVal* items = jget(root, "items");
    if (!items || !items->IsArray() || items->Empty()) return false;
    const JVal& c = (*items)[0];
    p.channelId = jstr(jget(c, "id"));
    p.name = jstr(c, "snippet.title");
    p.handle = jstr(c, "snippet.customUrl");
    p.avatar = best_thumb(jpath(c, "snippet.thumbnails"));
    return !p.channelId.empty();
}

void parse_api_subscriptions(const JVal& root, std::vector<Subscription>& out, std::string& next) {
    next = jstr(jget(root, "nextPageToken"));
    const JVal* items = jget(root, "items");
    if (!items || !items->IsArray()) return;
    for (auto& it : items->GetArray()) {
        Subscription s;
        s.id = jstr(it, "snippet.resourceId.channelId");
        s.name = jstr(it, "snippet.title");
        s.avatar = best_thumb(jpath(it, "snippet.thumbnails"));
        if (!s.id.empty()) out.push_back(s);
    }
}

void parse_api_playlists(const JVal& root, std::vector<Item>& out, std::string& next) {
    next = jstr(jget(root, "nextPageToken"));
    const JVal* items = jget(root, "items");
    if (!items || !items->IsArray()) return;
    for (auto& it : items->GetArray()) {
        Item p;
        p.type = ItemType::Playlist;
        p.id = jstr(jget(it, "id"));
        p.title = jstr(it, "snippet.title");
        p.channel = jstr(it, "snippet.channelTitle");
        p.channelId = jstr(it, "snippet.channelId");
        p.thumb = best_thumb(jpath(it, "snippet.thumbnails"));
        long long n = jint(it, "contentDetails.itemCount", -1);
        if (n >= 0) p.extra = n == 1 ? "1 video" : str_printf("%lld videos", n);
        if (!p.id.empty()) out.push_back(p);
    }
}

void parse_api_videos(const JVal& root, std::vector<Item>& out, std::string& next) {
    next = jstr(jget(root, "nextPageToken"));
    const JVal* items = jget(root, "items");
    if (!items || !items->IsArray()) return;
    for (auto& it : items->GetArray()) {
        Item v;
        v.type = ItemType::Video;
        const JVal* idv = jget(it, "id");
        if (idv && idv->IsString()) v.id = jstr(idv);  // videos.list
        std::string resId = jstr(it, "snippet.resourceId.videoId");  // playlistItems.list
        if (!resId.empty()) v.id = resId;
        v.title = jstr(it, "snippet.title");
        if (v.id.empty() || v.title == "Deleted video" || v.title == "Private video") continue;
        v.channel = jstr(it, "snippet.videoOwnerChannelTitle");
        v.channelId = jstr(it, "snippet.videoOwnerChannelId");
        if (resId.empty()) {  // videos.list: the snippet's channel is the uploader
            v.channel = jstr(it, "snippet.channelTitle");
            v.channelId = jstr(it, "snippet.channelId");
        }
        std::string dur = jstr(it, "contentDetails.duration");
        if (!dur.empty()) {
            int secs = iso8601_seconds(dur);
            if (secs > 0) v.duration = format_duration(secs);
        }
        long long views = jint(it, "statistics.viewCount", -1);
        if (views >= 0) v.views = format_views(views);
        std::string live = jstr(it, "snippet.liveBroadcastContent");
        if (live == "live") v.live = true;
        out.push_back(v);
    }
}

std::string parse_api_error(const JVal& root) {
    const JVal* e = jget(root, "error");
    if (!e) return std::string();
    if (e->IsString()) return jstr(e);
    std::string reason = jstr(*e, "errors.0.reason");
    std::string msg = jstr(jget(*e, "message"));
    if (reason == "quotaExceeded") return "The Google project's daily YouTube API quota is used up";
    if (msg.empty()) msg = reason.empty() ? "Google returned an error" : reason;
    return msg;
}

// ---------------------------------------------------------------- account

void GoogleAccount::init(const std::string& dir) {
    std::lock_guard<std::mutex> lock(m_);
    dir_ = dir;
    clientId_ = VT_GOOGLE_CLIENT_ID;
    clientSecret_ = VT_GOOGLE_CLIENT_SECRET;
    std::string text, err;
    if (read_file(dir + "/google_client.json", text)) {
        if (JDocPtr d = jparse(text, &err)) {
            // Accept Google's downloaded client file ({"installed": {...}}) or a flat object.
            const JVal* c = jget(*d, "installed");
            if (!c) c = d.get();
            std::string id = jstr(jget(*c, "client_id")), secret = jstr(jget(*c, "client_secret"));
            if (!id.empty()) {
                clientId_ = id;
                clientSecret_ = secret;
            }
        } else {
            VT_LOG("google: google_client.json: %s", err.c_str());
        }
    }
    if (read_file(dir + "/account.json", text)) {
        if (JDocPtr d = jparse(text, &err)) {
            refresh_ = jstr(jget(*d, "refresh"));
            profile_.channelId = jstr(jget(*d, "channelId"));
            profile_.name = jstr(jget(*d, "name"));
            profile_.handle = jstr(jget(*d, "handle"));
            profile_.avatar = jstr(jget(*d, "avatar"));
        }
    }
    VT_LOG("google: client %s, %s", clientId_.empty() ? "not set" : "set", refresh_.empty() ? "signed out" : "signed in");
}

bool GoogleAccount::configured() const {
    std::lock_guard<std::mutex> lock(m_);
    return !clientId_.empty();
}

bool GoogleAccount::signedIn() const {
    std::lock_guard<std::mutex> lock(m_);
    return !refresh_.empty();
}

GoogleProfile GoogleAccount::profile() const {
    std::lock_guard<std::mutex> lock(m_);
    return profile_;
}

void GoogleAccount::save() {
    // Caller holds m_.
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    auto s = [&](const char* k, const std::string& v) {
        w.Key(k);
        w.String(v.c_str());
    };
    s("refresh", refresh_);
    s("channelId", profile_.channelId);
    s("name", profile_.name);
    s("handle", profile_.handle);
    s("avatar", profile_.avatar);
    w.EndObject();
    if (!write_file_atomic(dir_ + "/account.json", std::string(sb.GetString(), sb.GetSize())))
        VT_LOG("google: failed to save account.json");
}

DeviceCode GoogleAccount::requestCode() {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(m_);
        id = clientId_;
    }
    DeviceCode c;
    if (id.empty()) {
        c.error = "No Google OAuth client is set up";
        return c;
    }
    HttpResponse r = form_post("https://oauth2.googleapis.com/device/code",
                               "client_id=" + url_encode(id) + "&scope=" + url_encode(kScope));
    if (!r.error.empty()) {
        c.error = r.describe();
        return c;
    }
    std::string err;
    JDocPtr d = jparse(r.body, &err);
    if (!d) {
        c.error = r.describe();
        return c;
    }
    return parse_device_code(*d);
}

TokenResult GoogleAccount::pollToken(const std::string& deviceCode) {
    std::string id, secret;
    {
        std::lock_guard<std::mutex> lock(m_);
        id = clientId_;
        secret = clientSecret_;
    }
    TokenResult t;
    HttpResponse r = form_post("https://oauth2.googleapis.com/token",
                               "client_id=" + url_encode(id) + "&client_secret=" + url_encode(secret) +
                                   "&device_code=" + url_encode(deviceCode) +
                                   "&grant_type=" + url_encode("urn:ietf:params:oauth:grant-type:device_code"));
    if (!r.error.empty()) {
        t.error = "authorization_pending";  // network hiccup: keep polling
        return t;
    }
    std::string err;
    JDocPtr d = jparse(r.body, &err);
    if (!d) {
        t.error = r.describe();
        return t;
    }
    return parse_token(*d);
}

bool GoogleAccount::completeSignIn(const TokenResult& t, std::string* err) {
    {
        std::lock_guard<std::mutex> lock(m_);
        access_ = t.access;
        accessExpiry_ = unix_now() + t.expiresIn - 60;
        if (!t.refresh.empty()) refresh_ = t.refresh;
        if (refresh_.empty()) {
            if (err) *err = "Google did not grant offline access";
            return false;
        }
    }
    JDocPtr d = api("GET", "channels?part=snippet&mine=true", "", err);
    GoogleProfile p;
    if (d) parse_profile(*d, p);
    std::lock_guard<std::mutex> lock(m_);
    profile_ = p;
    if (profile_.name.empty()) profile_.name = "Google account";
    save();
    return true;
}

void GoogleAccount::signOut() {
    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_);
        token = refresh_;
        refresh_.clear();
        access_.clear();
        accessExpiry_ = 0;
        profile_ = GoogleProfile();
        save();
    }
    if (!token.empty()) form_post("https://oauth2.googleapis.com/revoke", "token=" + url_encode(token));
}

bool GoogleAccount::accessToken(std::string& token, std::string* err) {
    std::string id, secret, refresh;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (refresh_.empty()) {
            if (err) *err = "Not signed in";
            return false;
        }
        if (!access_.empty() && unix_now() < accessExpiry_) {
            token = access_;
            return true;
        }
        id = clientId_;
        secret = clientSecret_;
        refresh = refresh_;
    }
    HttpResponse r = form_post("https://oauth2.googleapis.com/token",
                               "client_id=" + url_encode(id) + "&client_secret=" + url_encode(secret) +
                                   "&refresh_token=" + url_encode(refresh) + "&grant_type=refresh_token");
    std::string perr;
    JDocPtr d = r.error.empty() ? jparse(r.body, &perr) : nullptr;
    if (!d) {
        if (err) *err = r.describe();
        return false;
    }
    TokenResult t = parse_token(*d);
    if (!t.error.empty()) {
        if (t.error == "invalid_grant") {
            // Revoked or expired (Google expires refresh tokens of "Testing" projects after 7 days).
            std::lock_guard<std::mutex> lock(m_);
            refresh_.clear();
            save();
            if (err) *err = "Your Google sign-in expired. Sign in again.";
        } else if (err) {
            *err = "Google sign-in: " + t.error;
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(m_);
    access_ = t.access;
    accessExpiry_ = unix_now() + t.expiresIn - 60;
    token = access_;
    return true;
}

JDocPtr GoogleAccount::api(const std::string& method, const std::string& path, const std::string& body, std::string* err,
                           long* status) {
    std::string token;
    if (!accessToken(token, err)) return nullptr;
    HttpRequest req;
    req.url = kApi + path;
    req.method = method;
    req.body = body;
    req.headers.push_back("Authorization: Bearer " + token);
    if (!body.empty()) req.headers.push_back("Content-Type: application/json");
    else if (method == "POST") req.headers.push_back("Content-Length: 0");
    HttpResponse r = http_request(req);
    if (status) *status = r.status;
    if (!r.error.empty()) {
        if (err) *err = r.describe();
        return nullptr;
    }
    if (r.body.empty()) {
        if (r.ok()) return jparse("{}", nullptr);
        if (err) *err = r.describe();
        return nullptr;
    }
    std::string perr;
    JDocPtr d = jparse(r.body, &perr);
    if (!d) {
        if (err) *err = r.ok() ? perr : r.describe();
        return nullptr;
    }
    std::string apiErr = parse_api_error(*d);
    if (!apiErr.empty() || !r.ok()) {
        if (err) *err = apiErr.empty() ? r.describe() : apiErr;
        return nullptr;
    }
    return d;
}

bool GoogleAccount::mySubscriptions(std::vector<Subscription>& out, std::string* err) {
    std::string page;
    for (int i = 0; i < 20; i++) {  // up to 1000 channels
        std::string path = "subscriptions?part=snippet&mine=true&maxResults=50&order=alphabetical";
        if (!page.empty()) path += "&pageToken=" + url_encode(page);
        JDocPtr d = api("GET", path, "", err);
        if (!d) return false;
        parse_api_subscriptions(*d, out, page);
        if (page.empty()) break;
    }
    return true;
}

bool GoogleAccount::subscribe(const std::string& channelId, std::string* err) {
    std::string body = "{\"snippet\":{\"resourceId\":{\"kind\":\"youtube#channel\",\"channelId\":" +
                       json_string(channelId) + "}}}";
    long status = 0;
    JDocPtr d = api("POST", "subscriptions?part=snippet", body, err, &status);
    if (!d && err && contains(*err, "already")) {
        err->clear();
        return true;
    }
    return d != nullptr;
}

bool GoogleAccount::unsubscribe(const std::string& channelId, std::string* err) {
    JDocPtr d = api("GET", "subscriptions?part=id&mine=true&forChannelId=" + url_encode(channelId), "", err);
    if (!d) return false;
    std::string id = jstr(*d, "items.0.id");
    if (id.empty()) return true;  // not subscribed on the account
    return api("DELETE", "subscriptions?id=" + url_encode(id), "", err) != nullptr;
}

bool GoogleAccount::rate(const std::string& videoId, int rating, std::string* err) {
    const char* r = rating > 0 ? "like" : rating < 0 ? "dislike" : "none";
    return api("POST", "videos/rate?id=" + url_encode(videoId) + "&rating=" + r, "", err) != nullptr;
}

bool GoogleAccount::getRating(const std::string& videoId, int& rating, std::string* err) {
    JDocPtr d = api("GET", "videos/getRating?id=" + url_encode(videoId), "", err);
    if (!d) return false;
    std::string r = jstr(*d, "items.0.rating");
    rating = r == "like" ? 1 : r == "dislike" ? -1 : 0;
    return true;
}

bool GoogleAccount::myPlaylists(std::vector<Item>& out, std::string* err) {
    std::string page;
    for (int i = 0; i < 4; i++) {
        std::string path = "playlists?part=snippet,contentDetails&mine=true&maxResults=50";
        if (!page.empty()) path += "&pageToken=" + url_encode(page);
        JDocPtr d = api("GET", path, "", err);
        if (!d) return false;
        parse_api_playlists(*d, out, page);
        if (page.empty()) break;
    }
    return true;
}

Page GoogleAccount::playlistItems(const std::string& playlistId, const std::string& pageToken) {
    Page p;
    std::string path = "playlistItems?part=snippet&maxResults=50&playlistId=" + url_encode(playlistId);
    if (!pageToken.empty()) path += "&pageToken=" + url_encode(pageToken);
    JDocPtr d = api("GET", path, "", &p.error);
    if (d) parse_api_videos(*d, p.items, p.continuation);
    return p;
}

Page GoogleAccount::likedVideos(const std::string& pageToken) {
    Page p;
    std::string path = "videos?part=snippet,contentDetails,statistics&myRating=like&maxResults=50";
    if (!pageToken.empty()) path += "&pageToken=" + url_encode(pageToken);
    JDocPtr d = api("GET", path, "", &p.error);
    if (d) parse_api_videos(*d, p.items, p.continuation);
    return p;
}

bool GoogleAccount::addToPlaylist(const std::string& playlistId, const std::string& videoId, std::string* err) {
    std::string body = "{\"snippet\":{\"playlistId\":" + json_string(playlistId) +
                       ",\"resourceId\":{\"kind\":\"youtube#video\",\"videoId\":" + json_string(videoId) + "}}}";
    return api("POST", "playlistItems?part=snippet", body, err) != nullptr;
}

}  // namespace vt
