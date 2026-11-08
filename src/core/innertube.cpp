#include "innertube.h"

#include "http.h"
#include "parse.h"
#include "util.h"

namespace vt {

namespace {

const char* kBase = "https://www.youtube.com/youtubei/v1/";
const char* kChannelVideosParams = "EgZ2aWRlb3PyBgQKAjoA";
const char* kChannelPlaylistsParams = "EglwbGF5bGlzdHPyBgQKAkIA";

void load_profile_file(const JVal& root, std::map<std::string, ClientProfile>& profiles,
                       std::map<std::string, std::string>& roles, std::vector<int>& vItags, std::vector<int>& aItags) {
    if (const JVal* c = jget(root, "clients")) {
        if (c->IsObject()) {
            for (auto& m : c->GetObject()) {
                ClientProfile p;
                p.name = m.name.GetString();
                p.id = (int)jint(jget(m.value, "clientId"), 0);
                p.userAgent = jstr(jget(m.value, "userAgent"));
                if (const JVal* ctx = jget(m.value, "context")) {
                    p.contextJson = jserialize(*ctx);
                    p.version = jstr(jget(*ctx, "clientVersion"));
                }
                if (const JVal* h = jget(m.value, "headers")) {
                    if (h->IsObject())
                        for (auto& hh : h->GetObject())
                            p.headers.push_back(std::string(hh.name.GetString()) + ": " + jstr(&hh.value));
                }
                profiles[p.name] = p;
            }
        }
    }
    if (const JVal* r = jget(root, "roles")) {
        if (r->IsObject())
            for (auto& m : r->GetObject()) roles[m.name.GetString()] = jstr(&m.value);
    }
    auto ints = [](const JVal* a, std::vector<int>& out) {
        if (!a || !a->IsArray() || a->Empty()) return;
        out.clear();
        for (auto& v : a->GetArray()) out.push_back((int)jint(&v));
    };
    ints(jget(root, "preferredVideoItags"), vItags);
    ints(jget(root, "preferredAudioItags"), aItags);
}

}  // namespace

bool InnerTube::loadProfiles(const std::string& builtinPath, const std::string& overridePath, std::string* err) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    if (!read_file(builtinPath, text)) {
        if (err) *err = "cannot read " + builtinPath;
        return false;
    }
    JDocPtr d = jparse(text, err);
    if (!d) return false;
    load_profile_file(*d, profiles_, roles_, videoItags_, audioItags_);
    if (!overridePath.empty() && read_file(overridePath, text)) {
        std::string e2;
        if (JDocPtr o = jparse(text, &e2)) {
            load_profile_file(*o, profiles_, roles_, videoItags_, audioItags_);
            VT_LOG("innertube: applied override %s", overridePath.c_str());
        } else {
            VT_LOG("innertube: ignoring bad override: %s", e2.c_str());
        }
    }
    return !profiles_.empty();
}

void InnerTube::setLocale(const std::string& hl, const std::string& gl) {
    std::lock_guard<std::mutex> lock(mutex_);
    hl_ = hl;
    gl_ = gl;
}

const ClientProfile* InnerTube::profileFor(const std::string& role) {
    auto r = roles_.find(role);
    std::string name = r != roles_.end() ? r->second : "WEB";
    auto p = profiles_.find(name);
    if (p == profiles_.end()) p = profiles_.find("WEB");
    return p == profiles_.end() ? nullptr : &p->second;
}

std::string InnerTube::buildBody(const std::string& role, const std::string& extraJson) {
    std::string ctx, hl, gl, visitor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const ClientProfile* p = profileFor(role);
        ctx = p ? p->contextJson : "{}";
        hl = hl_;
        gl = gl_;
        visitor = visitor_;
    }
    JDoc client;
    client.Parse(ctx.c_str());
    if (!client.IsObject()) client.SetObject();
    auto& a = client.GetAllocator();
    client.RemoveMember("hl");
    client.RemoveMember("gl");
    client.AddMember("hl", JVal(hl.c_str(), a), a);
    client.AddMember("gl", JVal(gl.c_str(), a), a);
    if (!visitor.empty()) {
        client.RemoveMember("visitorData");
        client.AddMember("visitorData", JVal(visitor.c_str(), a), a);
    }
    JDoc body;
    body.SetObject();
    auto& ba = body.GetAllocator();
    JVal context(rapidjson::kObjectType);
    JVal clientCopy(client, ba);
    context.AddMember("client", clientCopy, ba);
    body.AddMember("context", context, ba);
    if (!extraJson.empty()) {
        JDoc extra;
        extra.Parse(extraJson.c_str());
        if (extra.IsObject()) {
            for (auto& m : extra.GetObject()) {
                JVal k(m.name, ba);
                JVal v(m.value, ba);
                body.AddMember(k, v, ba);
            }
        }
    }
    return jserialize(body);
}

bool InnerTube::ensureVisitor(std::string* err) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!visitor_.empty()) return true;
    }
    // One request for every caller: the first fetches, the rest wait here and reuse its result.
    std::lock_guard<std::mutex> fetch(visitorFetch_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!visitor_.empty()) return true;
    }
    // Without visitorData the player endpoint answers LOGIN_REQUIRED ("confirm you're not a bot").
    std::string e;
    JDocPtr d = call("visitor", "visitor_id", std::string(), &e);
    std::string v = d ? jstr(*d, "responseContext.visitorData") : std::string();
    if (v.empty()) {
        if (err) *err = "could not get visitor ID" + (e.empty() ? std::string() : ": " + e);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    visitor_ = v;
    VT_LOG("innertube: visitorData acquired");
    return true;
}

JDocPtr InnerTube::call(const std::string& role, const char* endpoint, const std::string& extraJson, std::string* err,
                        const std::vector<std::string>& extraHeaders) {
    if (std::string(endpoint) != "visitor_id" && !ensureVisitor(err)) return nullptr;
    HttpRequest req;
    std::string visitor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const ClientProfile* p = profileFor(role);
        if (!p) {
            if (err) *err = "no client profile for " + role;
            return nullptr;
        }
        req.userAgent = p->userAgent;
        req.headers = p->headers;
        req.headers.push_back(str_printf("X-YouTube-Client-Name: %d", p->id));
        req.headers.push_back("X-YouTube-Client-Version: " + p->version);
        visitor = visitor_;
    }
    req.url = std::string(kBase) + endpoint + "?prettyPrint=false";
    req.method = "POST";
    req.body = buildBody(role, extraJson);
    req.headers.push_back("Content-Type: application/json");
    req.headers.push_back("Origin: https://www.youtube.com");
    if (!visitor.empty()) req.headers.push_back("X-Goog-Visitor-Id: " + visitor);
    for (const std::string& h : extraHeaders) req.headers.push_back(h);

    double t0 = monotonic_seconds();
    HttpResponse res = http_request(req);
    VT_LOG("innertube: %s/%s -> %s (%u bytes, %.2fs)", role.c_str(), endpoint, res.describe().c_str(),
           (unsigned)res.body.size(), monotonic_seconds() - t0);
    if (!res.error.empty()) {
        if (err) *err = "network error: " + res.error;
        return nullptr;
    }
    JDocPtr doc = jparse(res.body, err);
    if (!doc) return nullptr;
    if (!res.ok()) {
        std::string msg = jstr(*doc, "error.message");
        if (err) *err = "YouTube rejected the request (" + res.describe() + (msg.empty() ? "" : ": " + msg) + ")";
        return nullptr;
    }
    return doc;
}

std::string search_params(const SearchFilters& f) {
    std::string sub;
    auto field = [](std::string& out, int tag, int v) {
        out += (char)(tag << 3);  // varint wire type
        out += (char)v;           // every value fits in one byte
    };
    if (f.upload) field(sub, 1, f.upload);
    if (f.type) field(sub, 2, f.type);
    if (f.duration) field(sub, 3, f.duration);
    if (f.hd) field(sub, 4, 1);
    if (f.cc) field(sub, 5, 1);
    if (f.live) field(sub, 8, 1);
    if (f.uhd) field(sub, 14, 1);
    std::string msg;
    if (f.sort) field(msg, 1, f.sort);
    if (!sub.empty()) {
        msg += (char)0x12;  // field 2, length-delimited
        msg += (char)sub.size();
        msg += sub;
    }
    if (msg.empty()) return "";
    static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b;
    for (size_t i = 0; i < msg.size(); i += 3) {
        uint32_t n = (uint8_t)msg[i] << 16;
        if (i + 1 < msg.size()) n |= (uint8_t)msg[i + 1] << 8;
        if (i + 2 < msg.size()) n |= (uint8_t)msg[i + 2];
        b += kB64[(n >> 18) & 63];
        b += kB64[(n >> 12) & 63];
        b += i + 1 < msg.size() ? kB64[(n >> 6) & 63] : '=';
        b += i + 2 < msg.size() ? kB64[n & 63] : '=';
    }
    return url_encode(b);
}

Page InnerTube::search(const std::string& query, const std::string& continuation, const std::string& params) {
    std::string err;
    std::string extra;
    if (!continuation.empty()) extra = "{\"continuation\":" + json_string(continuation) + "}";
    else if (!params.empty()) extra = "{\"query\":" + json_string(query) + ",\"params\":" + json_string(params) + "}";
    else extra = "{\"query\":" + json_string(query) + "}";
    JDocPtr d = call("search", "search", extra, &err);
    Page p;
    if (d) p = parse_search(*d);
    if (!err.empty()) p.error = err;
    return p;
}

ChannelPage InnerTube::channel(const std::string& channelId, ChannelTab tab, const std::string& continuation) {
    std::string err, extra;
    if (!continuation.empty()) {
        extra = "{\"continuation\":" + json_string(continuation) + "}";
    } else {
        extra = "{\"browseId\":" + json_string(channelId);
        if (tab == ChannelTab::Videos) extra += std::string(",\"params\":\"") + kChannelVideosParams + "\"";
        if (tab == ChannelTab::Playlists) extra += std::string(",\"params\":\"") + kChannelPlaylistsParams + "\"";
        extra += "}";
    }
    JDocPtr d = call("browse", "browse", extra, &err);
    ChannelPage p;
    if (d) p = parse_channel(*d);
    if (!err.empty()) p.error = err;
    if (p.id.empty()) p.id = channelId;
    return p;
}

PlaylistPage InnerTube::playlist(const std::string& playlistId, const std::string& continuation) {
    std::string err;
    std::string extra = continuation.empty() ? "{\"browseId\":" + json_string("VL" + playlistId) + "}"
                                             : "{\"continuation\":" + json_string(continuation) + "}";
    JDocPtr d = call("browse", "browse", extra, &err);
    PlaylistPage p;
    if (d) p = parse_playlist(*d);
    if (!err.empty()) p.error = err;
    if (p.id.empty()) p.id = playlistId;
    return p;
}

Page InnerTube::topicFeed(const std::string& channelId) {
    std::string err;
    JDocPtr d = call("browse", "browse", "{\"browseId\":" + json_string(channelId) + "}", &err);
    Page p;
    if (d) p = parse_browse_feed(*d);
    if (!err.empty()) p.error = err;
    return p;
}

Page InnerTube::home(const std::string& bearer, const std::string& continuation, bool* personal) {
    std::string err;
    std::string extra = continuation.empty() ? std::string("{\"browseId\":\"FEwhat_to_watch\"}")
                                             : "{\"continuation\":" + json_string(continuation) + "}";
    std::vector<std::string> headers;
    if (!bearer.empty()) headers.push_back("Authorization: Bearer " + bearer);
    JDocPtr d = call("browse", "browse", extra, &err, headers);
    Page p;
    bool loggedOut = true;
    if (d) p = parse_home_feed(*d, &loggedOut);
    if (!err.empty()) p.error = err;
    if (personal) *personal = d && !loggedOut && !p.items.empty();
    VT_LOG("innertube: home %s, %u videos, %s", bearer.empty() ? "signed out" : "with token", (unsigned)p.items.size(),
           loggedOut ? "not personalised" : "personalised");
    return p;
}

PlayerInfo InnerTube::player(const std::string& videoId) {
    std::string err;
    std::string extra = "{\"videoId\":" + json_string(videoId) + ",\"contentCheckOk\":true,\"racyCheckOk\":true}";
    JDocPtr d = call("player", "player", extra, &err);
    PlayerInfo info;
    if (d) info = parse_player(*d);
    if (!err.empty()) {
        info.status = "ERROR";
        info.reason = err;
    }
    return info;
}

WatchInfo InnerTube::next(const std::string& videoId) {
    std::string err;
    JDocPtr d = call("next", "next", "{\"videoId\":" + json_string(videoId) + "}", &err);
    WatchInfo w;
    if (d) w = parse_next(*d);
    if (!err.empty()) w.error = err;
    return w;
}

WatchInfo InnerTube::relatedMore(const std::string& continuation) {
    std::string err;
    JDocPtr d = call("next", "next", "{\"continuation\":" + json_string(continuation) + "}", &err);
    WatchInfo w;
    if (d) w = parse_next(*d);
    if (!err.empty()) w.error = err;
    return w;
}

CommentsPage InnerTube::comments(const std::string& continuation) {
    std::string err;
    JDocPtr d = call("next", "next", "{\"continuation\":" + json_string(continuation) + "}", &err);
    CommentsPage c;
    if (d) c = parse_comments(*d);
    if (!err.empty()) c.error = err;
    return c;
}

ReelSeed InnerTube::reelSeed() {
    std::string err;
    JDocPtr d = call("browse", "reel/reel_item_watch",
                     "{\"inputType\":\"REEL_WATCH_INPUT_TYPE_SEEDLESS\",\"params\":\"CA8%3D\","
                     "\"disablePlayerResponse\":true}",
                     &err);
    ReelSeed r;
    if (d) r = parse_reel_seed(*d);
    if (!err.empty()) r.error = err;
    return r;
}

Page InnerTube::reelSequence(const std::string& sequence) {
    std::string err;
    JDocPtr d = call("browse", "reel/reel_watch_sequence", "{\"sequenceParams\":" + json_string(sequence) + "}", &err);
    Page p;
    if (d) p = parse_reel_sequence(*d);
    if (!err.empty()) p.error = err;
    return p;
}

std::vector<Item> InnerTube::channelRss(const std::string& channelId, std::string* err) {
    HttpResponse r = http_get("https://www.youtube.com/feeds/videos.xml?channel_id=" + url_encode(channelId));
    if (!r.ok()) {
        if (err) *err = r.describe();
        return {};
    }
    return parse_rss(r.body);
}

std::string InnerTube::playerUserAgent() {
    std::lock_guard<std::mutex> lock(mutex_);
    const ClientProfile* p = profileFor("player");
    return p ? p->userAgent : std::string();
}

bool InnerTube::captions(const std::string& baseUrl, std::string& out, std::string* err) {
    std::string url = baseUrl;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "fmt=json3";
    std::string ua;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const ClientProfile* p = profileFor("player")) ua = p->userAgent;
    }
    HttpResponse r = http_get(url, ua);
    if (!r.ok() || r.body.empty()) {
        if (err) *err = r.body.empty() && r.ok() ? "empty captions response" : r.describe();
        return false;
    }
    out = std::move(r.body);
    return true;
}

}  // namespace vt
