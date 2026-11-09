#include "store.h"

#include <algorithm>

#include "json_util.h"
#include "util.h"

namespace vt {

namespace {

const size_t kMaxHistory = 300;
const size_t kMaxSearches = 20;

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

void write_item(Writer& w, const Item& it) {
    w.StartObject();
    w.Key("type");
    w.Int(it.type == ItemType::Video ? 0 : it.type == ItemType::Playlist ? 1 : 2);
    auto s = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        w.Key(k);
        w.String(v.c_str(), (rapidjson::SizeType)v.size());
    };
    s("id", it.id);
    s("title", it.title);
    s("channel", it.channel);
    s("channelId", it.channelId);
    s("duration", it.duration);
    s("views", it.views);
    s("published", it.published);
    s("thumb", it.thumb);
    s("avatar", it.avatar);
    s("extra", it.extra);
    if (it.time) {
        w.Key("time");
        w.Int64(it.time);
    }
    if (it.shorts) {
        w.Key("shorts");
        w.Bool(true);
    }
    if (it.verified) {
        w.Key("verified");
        w.Bool(true);
    }
    w.EndObject();
}

Item read_item(const JVal& v) {
    Item it;
    int t = (int)jint(jget(v, "type"), 0);
    it.type = t == 1 ? ItemType::Playlist : t == 2 ? ItemType::Channel : ItemType::Video;
    it.id = jstr(jget(v, "id"));
    it.title = jstr(jget(v, "title"));
    it.channel = jstr(jget(v, "channel"));
    it.channelId = jstr(jget(v, "channelId"));
    it.duration = jstr(jget(v, "duration"));
    it.views = jstr(jget(v, "views"));
    it.published = jstr(jget(v, "published"));
    it.thumb = jstr(jget(v, "thumb"));
    it.avatar = jstr(jget(v, "avatar"));
    it.extra = jstr(jget(v, "extra"));
    it.time = jint(jget(v, "time"), 0);
    it.shorts = jbool(v, "shorts");
    it.verified = jbool(v, "verified");
    return it;
}

// Loads path, or path.tmp when a save was interrupted.
JDocPtr load_json(const std::string& path) {
    std::string text;
    if (!read_file(path, text) && !read_file(path + ".tmp", text)) return nullptr;
    std::string err;
    JDocPtr d = jparse(text, &err);
    if (!d) VT_LOG("store: %s: %s", path.c_str(), err.c_str());
    return d;
}

void save(const std::string& path, const rapidjson::StringBuffer& sb) {
    if (!write_file_atomic(path, std::string(sb.GetString(), sb.GetSize())))
        VT_LOG("store: failed to write %s", path.c_str());
}

}  // namespace

Store::Store(std::string dir) : dir_(std::move(dir)) {}

std::string Store::itemToJson(const Item& it) {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    write_item(w, it);
    return std::string(sb.GetString(), sb.GetSize());
}

void Store::load() {
    subs.clear();
    history.clear();
    watchLater.clear();
    searches.clear();
    liked.clear();
    disliked.clear();
    playlists.clear();
    notInterested.clear();
    blockedChannels.clear();
    if (JDocPtr d = load_json(path("hidden.json"))) {
        if (const JVal* l = jget(*d, "videos"))
            if (l->IsArray())
                for (auto& v : l->GetArray())
                    if (v.IsString()) notInterested.push_back(v.GetString());
        if (const JVal* l = jget(*d, "channels"))
            if (l->IsArray())
                for (auto& v : l->GetArray()) {
                    Subscription s{jstr(jget(v, "id")), jstr(jget(v, "name")), ""};
                    if (!s.id.empty() || !s.name.empty()) blockedChannels.push_back(s);
                }
    }
    if (JDocPtr d = load_json(path("ratings.json"))) {
        if (const JVal* l = jget(*d, "liked"))
            if (l->IsArray())
                for (auto& v : l->GetArray()) {
                    Item it = read_item(v);
                    if (!it.id.empty()) liked.push_back(it);
                }
        if (const JVal* l = jget(*d, "disliked"))
            if (l->IsArray())
                for (auto& v : l->GetArray())
                    if (v.IsString()) disliked.push_back(v.GetString());
    }
    if (JDocPtr d = load_json(path("playlists.json"))) {
        if (d->IsArray())
            for (auto& v : d->GetArray()) {
                LocalPlaylist pl;
                pl.id = jstr(jget(v, "id"));
                pl.title = jstr(jget(v, "title"));
                pl.updated = jint(jget(v, "updated"), 0);
                if (const JVal* items = jget(v, "items"))
                    if (items->IsArray())
                        for (auto& iv : items->GetArray()) {
                            Item it = read_item(iv);
                            if (!it.id.empty()) pl.items.push_back(it);
                        }
                if (!pl.id.empty()) playlists.push_back(pl);
            }
    }
    if (JDocPtr d = load_json(path("subscriptions.json"))) {
        if (d->IsArray())
            for (auto& v : d->GetArray()) {
                Subscription s{jstr(jget(v, "id")), jstr(jget(v, "name")), jstr(jget(v, "avatar"))};
                if (!s.id.empty()) subs.push_back(s);
            }
    }
    if (JDocPtr d = load_json(path("history.json"))) {
        if (d->IsArray())
            for (auto& v : d->GetArray()) {
                HistoryEntry h;
                if (const JVal* it = jget(v, "item")) h.item = read_item(*it);
                h.watchedAt = jint(jget(v, "at"), 0);
                h.position = (int)jint(jget(v, "pos"), 0);
                h.length = (int)jint(jget(v, "len"), 0);
                if (!h.item.id.empty()) history.push_back(h);
            }
    }
    if (JDocPtr d = load_json(path("watch_later.json"))) {
        if (d->IsArray())
            for (auto& v : d->GetArray()) {
                Item it = read_item(v);
                if (!it.id.empty()) watchLater.push_back(it);
            }
    }
    if (JDocPtr d = load_json(path("searches.json"))) {
        if (d->IsArray())
            for (auto& v : d->GetArray())
                if (v.IsString()) searches.push_back(v.GetString());
    }
    if (JDocPtr d = load_json(path("settings.json"))) {
        settings.maxHeight = (int)jint(jget(*d, "maxHeight"), settings.maxHeight);
        settings.captions = jbool(*d, "captions", settings.captions);
        std::string v = jstr(jget(*d, "captionLang"));
        if (!v.empty()) settings.captionLang = v;
        v = jstr(jget(*d, "hl"));
        if (!v.empty()) settings.hl = v;
        v = jstr(jget(*d, "gl"));
        if (!v.empty()) settings.gl = v;
        settings.autoplay = jbool(*d, "autoplay", settings.autoplay);
        settings.rememberPosition = jbool(*d, "rememberPosition", settings.rememberPosition);
        settings.checkUpdates = jbool(*d, "checkUpdates", settings.checkUpdates);
        settings.skipVersion = jstr(jget(*d, "skipVersion"));
    }
    VT_LOG("store: %u subs, %u history, %u watch later", (unsigned)subs.size(), (unsigned)history.size(),
           (unsigned)watchLater.size());
}

bool Store::isSubscribed(const std::string& id) const {
    return std::any_of(subs.begin(), subs.end(), [&](const Subscription& s) { return s.id == id; });
}

void Store::subscribe(const std::string& id, const std::string& name, const std::string& avatar) {
    if (id.empty() || isSubscribed(id)) return;
    subs.push_back({id, name, avatar});
    std::sort(subs.begin(), subs.end(), [](const Subscription& a, const Subscription& b) { return a.name < b.name; });
    saveSubs();
}

void Store::unsubscribe(const std::string& id) {
    subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const Subscription& s) { return s.id == id; }), subs.end());
    saveSubs();
}

void Store::recordWatch(const Item& item, int position, int length) {
    if (item.id.empty()) return;
    removeHistory(item.id);
    HistoryEntry h;
    h.item = item;
    h.watchedAt = unix_now();
    h.position = position;
    h.length = length;
    history.insert(history.begin(), h);
    if (history.size() > kMaxHistory) history.resize(kMaxHistory);
    saveHistory();
}

int Store::resumePosition(const std::string& videoId) const {
    for (const HistoryEntry& h : history) {
        if (h.item.id != videoId) continue;
        // Treat the last 15 seconds (or 95%) as finished.
        if (h.length > 0 && (h.position >= h.length - 15 || h.position >= h.length * 95 / 100)) return 0;
        return h.position > 5 ? h.position : 0;
    }
    return 0;
}

float Store::watchedFraction(const std::string& videoId) const {
    if (videoId.empty()) return -1;
    for (const HistoryEntry& h : history) {
        if (h.item.id != videoId) continue;
        if (h.length <= 0 || h.position < 5) return -1;
        if (resumePosition(videoId) == 0) return 1.0f;  // finished
        return std::min(1.0f, (float)h.position / h.length);
    }
    return -1;
}

void Store::removeHistory(const std::string& id) {
    history.erase(std::remove_if(history.begin(), history.end(), [&](const HistoryEntry& h) { return h.item.id == id; }),
                  history.end());
}

void Store::clearHistory() {
    history.clear();
    saveHistory();
}

bool Store::inWatchLater(const std::string& id) const {
    return std::any_of(watchLater.begin(), watchLater.end(), [&](const Item& i) { return i.id == id; });
}

void Store::toggleWatchLater(const Item& item) {
    if (inWatchLater(item.id)) {
        removeWatchLater(item.id);
        return;
    }
    watchLater.insert(watchLater.begin(), item);
    saveWatchLater();
}

void Store::removeWatchLater(const std::string& id) {
    watchLater.erase(std::remove_if(watchLater.begin(), watchLater.end(), [&](const Item& i) { return i.id == id; }),
                     watchLater.end());
    saveWatchLater();
}

void Store::addSearch(const std::string& q) {
    std::string t = trim(q);
    if (t.empty()) return;
    searches.erase(std::remove(searches.begin(), searches.end(), t), searches.end());
    searches.insert(searches.begin(), t);
    if (searches.size() > kMaxSearches) searches.resize(kMaxSearches);
    saveSearches();
}

void Store::clearSearches() {
    searches.clear();
    saveSearches();
}

void Store::saveSubs() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartArray();
    for (const Subscription& s : subs) {
        w.StartObject();
        w.Key("id");
        w.String(s.id.c_str());
        w.Key("name");
        w.String(s.name.c_str());
        w.Key("avatar");
        w.String(s.avatar.c_str());
        w.EndObject();
    }
    w.EndArray();
    save(path("subscriptions.json"), sb);
}

void Store::saveHistory() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartArray();
    for (const HistoryEntry& h : history) {
        w.StartObject();
        w.Key("item");
        write_item(w, h.item);
        w.Key("at");
        w.Int64(h.watchedAt);
        w.Key("pos");
        w.Int(h.position);
        w.Key("len");
        w.Int(h.length);
        w.EndObject();
    }
    w.EndArray();
    save(path("history.json"), sb);
}

void Store::saveWatchLater() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartArray();
    for (const Item& it : watchLater) write_item(w, it);
    w.EndArray();
    save(path("watch_later.json"), sb);
}

void Store::saveSearches() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartArray();
    for (const std::string& s : searches) w.String(s.c_str());
    w.EndArray();
    save(path("searches.json"), sb);
}

void Store::saveSettings() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartObject();
    w.Key("maxHeight");
    w.Int(settings.maxHeight);
    w.Key("captions");
    w.Bool(settings.captions);
    w.Key("captionLang");
    w.String(settings.captionLang.c_str());
    w.Key("hl");
    w.String(settings.hl.c_str());
    w.Key("gl");
    w.String(settings.gl.c_str());
    w.Key("autoplay");
    w.Bool(settings.autoplay);
    w.Key("rememberPosition");
    w.Bool(settings.rememberPosition);
    w.Key("checkUpdates");
    w.Bool(settings.checkUpdates);
    w.Key("skipVersion");
    w.String(settings.skipVersion.c_str());
    w.EndObject();
    save(path("settings.json"), sb);
}

int Store::rating(const std::string& id) const {
    if (std::any_of(liked.begin(), liked.end(), [&](const Item& i) { return i.id == id; })) return 1;
    if (std::find(disliked.begin(), disliked.end(), id) != disliked.end()) return -1;
    return 0;
}

void Store::setRating(const Item& item, int r) {
    if (item.id.empty()) return;
    liked.erase(std::remove_if(liked.begin(), liked.end(), [&](const Item& i) { return i.id == item.id; }), liked.end());
    disliked.erase(std::remove(disliked.begin(), disliked.end(), item.id), disliked.end());
    if (r > 0) liked.insert(liked.begin(), item);
    if (r < 0) disliked.push_back(item.id);
    if (disliked.size() > 1000) disliked.erase(disliked.begin());
    saveRatings();
}

bool Store::channelBlocked(const std::string& channelId, const std::string& name) const {
    for (const Subscription& s : blockedChannels) {
        if (!channelId.empty() && !s.id.empty()) {
            if (s.id == channelId) return true;
        } else if (!name.empty() && s.name == name) {
            return true;
        }
    }
    return false;
}

bool Store::hidden(const Item& it) const {
    if (it.type == ItemType::Video &&
        std::find(notInterested.begin(), notInterested.end(), it.id) != notInterested.end())
        return true;
    if (blockedChannels.empty()) return false;
    if (it.type == ItemType::Channel) return channelBlocked(it.id, it.title);
    return channelBlocked(it.channelId, it.channel);
}

void Store::markNotInterested(const std::string& videoId) {
    if (videoId.empty()) return;
    notInterested.erase(std::remove(notInterested.begin(), notInterested.end(), videoId), notInterested.end());
    notInterested.insert(notInterested.begin(), videoId);
    if (notInterested.size() > 1000) notInterested.resize(1000);
    saveHidden();
}

void Store::blockChannel(const std::string& channelId, const std::string& name) {
    if ((channelId.empty() && name.empty()) || channelBlocked(channelId, name)) return;
    blockedChannels.insert(blockedChannels.begin(), Subscription{channelId, name, ""});
    saveHidden();
}

void Store::unblockChannel(size_t index) {
    if (index >= blockedChannels.size()) return;
    blockedChannels.erase(blockedChannels.begin() + index);
    saveHidden();
}

void Store::clearHidden() {
    notInterested.clear();
    blockedChannels.clear();
    saveHidden();
}

void Store::saveHidden() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartObject();
    w.Key("videos");
    w.StartArray();
    for (const std::string& id : notInterested) w.String(id.c_str());
    w.EndArray();
    w.Key("channels");
    w.StartArray();
    for (const Subscription& s : blockedChannels) {
        w.StartObject();
        w.Key("id");
        w.String(s.id.c_str());
        w.Key("name");
        w.String(s.name.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    save(path("hidden.json"), sb);
}

LocalPlaylist* Store::playlist(const std::string& id) {
    for (auto& p : playlists)
        if (p.id == id) return &p;
    return nullptr;
}

std::string Store::createPlaylist(const std::string& title) {
    LocalPlaylist pl;
    int64_t now = unix_now();
    pl.id = str_printf("VT%lld", (long long)now);
    while (playlist(pl.id)) pl.id += "x";
    pl.title = trim(title).empty() ? "Untitled playlist" : trim(title);
    pl.updated = now;
    playlists.insert(playlists.begin(), pl);
    savePlaylists();
    return pl.id;
}

void Store::deletePlaylist(const std::string& id) {
    playlists.erase(std::remove_if(playlists.begin(), playlists.end(), [&](const LocalPlaylist& p) { return p.id == id; }),
                    playlists.end());
    savePlaylists();
}

bool Store::inPlaylist(const std::string& pid, const std::string& vid) const {
    for (const auto& p : playlists)
        if (p.id == pid)
            return std::any_of(p.items.begin(), p.items.end(), [&](const Item& i) { return i.id == vid; });
    return false;
}

bool Store::togglePlaylistItem(const std::string& pid, const Item& item) {
    LocalPlaylist* p = playlist(pid);
    if (!p || item.id.empty()) return false;
    bool added = false;
    auto it = std::find_if(p->items.begin(), p->items.end(), [&](const Item& i) { return i.id == item.id; });
    if (it != p->items.end()) {
        p->items.erase(it);
    } else {
        p->items.push_back(item);
        added = true;
    }
    p->updated = unix_now();
    // Most recently changed first, like YouTube's "Save to" list.
    LocalPlaylist moved = *p;
    playlists.erase(std::remove_if(playlists.begin(), playlists.end(), [&](const LocalPlaylist& x) { return x.id == pid; }),
                    playlists.end());
    playlists.insert(playlists.begin(), moved);
    savePlaylists();
    return added;
}

void Store::saveRatings() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartObject();
    w.Key("liked");
    w.StartArray();
    for (const Item& it : liked) write_item(w, it);
    w.EndArray();
    w.Key("disliked");
    w.StartArray();
    for (const std::string& id : disliked) w.String(id.c_str());
    w.EndArray();
    w.EndObject();
    save(path("ratings.json"), sb);
}

void Store::savePlaylists() {
    rapidjson::StringBuffer sb;
    Writer w(sb);
    w.StartArray();
    for (const LocalPlaylist& p : playlists) {
        w.StartObject();
        w.Key("id");
        w.String(p.id.c_str());
        w.Key("title");
        w.String(p.title.c_str());
        w.Key("updated");
        w.Int64(p.updated);
        w.Key("items");
        w.StartArray();
        for (const Item& it : p.items) write_item(w, it);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    save(path("playlists.json"), sb);
}

}  // namespace vt
