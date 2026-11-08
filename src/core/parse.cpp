#include "parse.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "util.h"

namespace vt {

std::string Item::thumbUrl() const {
    if (type == ItemType::Video && !id.empty()) return "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
    return thumb;
}

static std::string fix_url(std::string u) {
    if (starts_with(u, "//")) u = "https:" + u;
    return u;
}

// First "url" inside a thumbnail container ({thumbnails:[...]} or {sources:[...]}), preferring
// the smallest image at least minWidth wide.
static std::string pick_thumb(const JVal* v, int minWidth = 160) {
    if (!v) return std::string();
    const JVal* list = jget(*v, "thumbnails");
    if (!list) list = jget(*v, "sources");
    if (!list || !list->IsArray() || list->Empty()) return std::string();
    std::string best, any;
    int bestW = 1 << 30;
    for (auto& t : list->GetArray()) {
        std::string u = jstr(jget(t, "url"));
        if (u.empty() || contains(u, ".webp") || contains(u, "an_webp")) continue;
        if (any.empty()) any = u;
        int w = (int)jint(jget(t, "width"), 0);
        if (w >= minWidth && w < bestW) {
            bestW = w;
            best = u;
        }
    }
    return fix_url(best.empty() ? any : best);
}

static std::string browse_id(const JVal* v) {
    if (!v) return std::string();
    const JVal* b = jfind(*v, "browseEndpoint", 24);
    return b ? jstr(jget(*b, "browseId")) : std::string();
}

static bool has_verified_badge(const JVal* badges) {
    if (!badges) return false;
    std::string b = jserialize(*badges);
    return contains(b, "BADGE_STYLE_TYPE_VERIFIED") || contains(b, "CHECK_CIRCLE");
}

static bool parse_video_renderer(const JVal& v, Item& it) {
    it.type = ItemType::Video;
    it.id = jstr(jget(v, "videoId"));
    if (it.id.empty()) return false;
    it.title = jtext(jget(v, "title"));
    if (it.title.empty()) it.title = jtext(jget(v, "headline"));
    const JVal* by = jget(v, "shortBylineText");
    if (!by) by = jget(v, "longBylineText");
    if (!by) by = jget(v, "ownerText");
    it.channel = jtext(by);
    it.channelId = browse_id(by);
    if (const JVal* ct = jfind(v, "channelThumbnailWithLinkRenderer", 4)) it.avatar = pick_thumb(jget(*ct, "thumbnail"), 48);
    else if (const JVal* ct2 = jget(v, "channelThumbnail")) it.avatar = pick_thumb(ct2, 48);
    it.duration = jtext(jget(v, "lengthText"));
    if (it.duration.empty()) {
        if (const JVal* ov = jfind(v, "thumbnailOverlayTimeStatusRenderer", 8)) {
            it.duration = jtext(jget(*ov, "text"));
            if (jstr(*ov, "style") == "LIVE") it.live = true;
        }
    }
    it.views = jtext(jget(v, "shortViewCountText"));
    if (it.views.empty()) it.views = jtext(jget(v, "viewCountText"));
    it.published = jtext(jget(v, "publishedTimeText"));
    if (const JVal* info = jget(v, "videoInfo")) {  // playlistVideoRenderer: "1.2M views • 3 years ago"
        const JVal* runs = jget(*info, "runs");
        if (runs && runs->IsArray() && runs->Size() >= 3) {
            if (it.views.empty()) it.views = jstr(jpath(*runs, "0.text"));
            if (it.published.empty()) it.published = jstr(jpath(*runs, "2.text"));
        } else if (it.views.empty()) {
            it.views = jtext(info);
        }
    }
    if (const JVal* badges = jget(v, "badges")) {
        std::string b = jserialize(*badges);
        if (contains(b, "LIVE_NOW")) it.live = true;
        if (badges->IsArray()) {
            for (auto& bd : badges->GetArray()) {
                std::string label = jstr(bd, "metadataBadgeRenderer.label");
                // Short labels only ("4K", "CC", "New"); skip shopping and the like.
                if (!label.empty() && label.size() <= 8 && !contains(label, "product")) it.badges.push_back(label);
            }
        }
    }
    it.verified = has_verified_badge(jget(v, "ownerBadges"));
    if (const JVal* sn = jget(v, "detailedMetadataSnippets")) {
        if (sn->IsArray() && !sn->Empty()) it.snippet = jtext(jget((*sn)[0], "snippetText"));
    }
    if (it.snippet.empty()) it.snippet = jtext(jget(v, "descriptionSnippet"));
    if (contains(it.views, "watching")) it.live = true;
    return !it.title.empty();
}

static bool parse_playlist_renderer(const JVal& v, Item& it) {
    it.type = ItemType::Playlist;
    it.id = jstr(jget(v, "playlistId"));
    if (it.id.empty() || starts_with(it.id, "RD")) return false;  // mixes cannot be browsed
    it.title = jtext(jget(v, "title"));
    std::string count = jstr(jget(v, "videoCount"));
    it.extra = count.empty() ? jtext(jget(v, "videoCountText")) : count + " videos";
    const JVal* by = jget(v, "shortBylineText");
    if (!by) by = jget(v, "longBylineText");
    it.channel = jtext(by);
    it.channelId = browse_id(by);
    const JVal* th = jget(v, "thumbnails");
    if (th && th->IsArray() && !th->Empty()) it.thumb = pick_thumb(&(*th)[0]);
    if (it.thumb.empty()) it.thumb = pick_thumb(jget(v, "thumbnail"));
    return !it.title.empty();
}

static bool parse_channel_renderer(const JVal& v, Item& it) {
    it.type = ItemType::Channel;
    it.id = jstr(jget(v, "channelId"));
    if (it.id.empty()) return false;
    it.title = jtext(jget(v, "title"));
    // YouTube now puts the @handle in subscriberCountText and the count in videoCountText.
    std::string a = jtext(jget(v, "subscriberCountText")), b = jtext(jget(v, "videoCountText"));
    it.extra = contains(b, "subscriber") ? b : (contains(a, "subscriber") ? a : (b.empty() ? a : b));
    it.thumb = pick_thumb(jget(v, "thumbnail"), 60);
    return !it.title.empty();
}

static bool parse_lockup(const JVal& v, Item& it) {
    std::string ctype = jstr(jget(v, "contentType"));
    it.id = jstr(jget(v, "contentId"));
    const JVal* meta = jpath(v, "metadata.lockupMetadataViewModel");
    if (!meta || it.id.empty()) return false;
    it.title = jtext(jget(*meta, "title"));

    std::vector<std::vector<std::string>> rows;
    if (const JVal* mr = jpath(*meta, "metadata.contentMetadataViewModel.metadataRows")) {
        if (mr->IsArray()) {
            for (auto& row : mr->GetArray()) {
                std::vector<std::string> parts;
                if (const JVal* mp = jget(row, "metadataParts")) {
                    if (mp->IsArray())
                        for (auto& p : mp->GetArray()) {
                            std::string t = jtext(jget(p, "text"));
                            if (!t.empty()) parts.push_back(t);
                        }
                }
                if (!parts.empty()) rows.push_back(parts);
            }
        }
    }

    std::string badge;
    const JVal* img = jget(v, "contentImage");
    if (img) {
        if (const JVal* b = jfind(*img, "thumbnailBadgeViewModel", 16)) badge = jstr(jget(*b, "text"));
    }
    it.channelId = browse_id(jget(*meta, "image"));
    if (const JVal* av = jfind(v, "avatarViewModel", 12)) it.avatar = pick_thumb(jget(*av, "image"), 48);
    if (it.channelId.empty()) {
        if (const JVal* md = jget(*meta, "metadata")) {
            std::string id = browse_id(md);
            if (starts_with(id, "UC")) it.channelId = id;
        }
    }

    if (const JVal* mr = jpath(*meta, "metadata.contentMetadataViewModel.metadataRows")) {
        if (mr->IsArray() && !mr->Empty()) {
            std::string first = jserialize((*mr)[0]);
            it.verified = contains(first, "CHECK_CIRCLE") || contains(first, "AUDIO_BADGE");
        }
    }
    if (ctype == "LOCKUP_CONTENT_TYPE_VIDEO") {
        it.type = ItemType::Video;
        it.duration = badge;
        if (badge == "LIVE" || badge == "UPCOMING") it.live = true;
        size_t statsRow = 0;
        if (rows.size() >= 2) {
            it.channel = rows[0][0];
            statsRow = 1;
        }
        if (statsRow < rows.size()) {
            for (const std::string& p : rows[statsRow]) {
                if (contains(p, "view") || contains(p, "watching") || (!p.empty() && isdigit((unsigned char)p[0]) &&
                                                                         parse_relative_age(p) < 0)) {
                    if (it.views.empty()) it.views = p;
                    if (contains(p, "watching")) it.live = true;
                } else if (it.published.empty()) {
                    it.published = p;
                }
            }
        }
        // Compact view counts ("1.5M") lack the word "views".
        if (!it.views.empty() && !contains(it.views, "view") && !contains(it.views, "watching"))
            it.views += " views";
        return !it.title.empty();
    }
    if (ctype == "LOCKUP_CONTENT_TYPE_PLAYLIST" || ctype == "LOCKUP_CONTENT_TYPE_ALBUM" ||
        ctype == "LOCKUP_CONTENT_TYPE_PODCAST") {
        if (starts_with(it.id, "RD")) return false;
        it.type = ItemType::Playlist;
        it.extra = badge;
        if (!rows.empty()) it.channel = rows[0][0];
        if (img) {
            const JVal* src = jfind(*img, "image", 12);
            it.thumb = pick_thumb(src);
        }
        return !it.title.empty();
    }
    return false;
}

// Shorts: shortsLockupViewModel (current) and reelItemRenderer (older shelves).
static bool parse_short(const char* key, const JVal& v, Item& it) {
    it = Item();
    it.type = ItemType::Video;
    it.shorts = true;
    if (!strcmp(key, "reelItemRenderer")) {
        it.id = jstr(jget(v, "videoId"));
        it.title = jtext(jget(v, "headline"));
        it.views = jtext(jget(v, "viewCountText"));
        it.thumb = pick_thumb(jget(v, "thumbnail"), 200);
    } else {
        if (const JVal* rw = jfind(v, "reelWatchEndpoint", 8)) it.id = jstr(jget(*rw, "videoId"));
        it.title = jstr(v, "overlayMetadata.primaryText.content");
        it.views = jstr(v, "overlayMetadata.secondaryText.content");
        if (const JVal* tv = jget(v, "thumbnailViewModel")) {
            if (const JVal* img = jfind(*tv, "image", 4)) it.thumb = pick_thumb(img, 200);
        }
        std::string badge = jstr(v, "badge.badgeViewModel.badgeText");
        if (!badge.empty()) it.badges.push_back(badge);
    }
    if (!it.views.empty() && !contains(it.views, "view")) it.views += " views";
    return !it.id.empty() && !it.title.empty();
}

static const char* const kItemKeys[] = {
    "videoRenderer",         "compactVideoRenderer",    "gridVideoRenderer", "videoWithContextRenderer",
    "playlistVideoRenderer", "playlistPanelVideoRenderer", "lockupViewModel", "playlistRenderer",
    "gridPlaylistRenderer",  "compactPlaylistRenderer", "channelRenderer",   "gridChannelRenderer",
};

bool parse_item(const char* key, const JVal& v, Item& out) {
    out = Item();
    if (!strcmp(key, "lockupViewModel")) return parse_lockup(v, out);
    if (strstr(key, "laylistRenderer")) return parse_playlist_renderer(v, out);
    if (strstr(key, "hannelRenderer")) return parse_channel_renderer(v, out);
    return parse_video_renderer(v, out);
}

static bool is_item_key(const char* key) {
    for (const char* k : kItemKeys)
        if (!strcmp(key, k)) return true;
    return false;
}

static const char* const kSkipKeys[] = {
    "responseContext", "topbar", "header", "frameworkUpdates", "engagementPanels", "microformat", "metadata",
    "sidebar", "adSlotRenderer",
    "promotedSparklesWebRenderer", "searchPyvRenderer", "chipCloudRenderer", "feedFilterChipBarRenderer",
    "playerOverlays", "trackingParams", "menu", "thumbnailOverlays", "onResponseReceivedEndpoints_ignore",
    "backgroundPromoRenderer", "messageRenderer", "commentsEntryPointHeaderRenderer",
};

static std::string continuation_token(const JVal& cir) {
    if (const JVal* c = jfind(cir, "continuationCommand", 12)) return jstr(jget(*c, "token"));
    if (const JVal* c = jfind(cir, "continuationEndpoint", 12)) {
        if (const JVal* t = jfind(*c, "token", 8)) return jstr(t);
    }
    return std::string();
}

void collect_items(const JVal& root, Page& page) {
    std::set<std::string> seen, seenShorts;
    for (const Item& i : page.items) seen.insert(i.id);
    for (const Item& i : page.shorts) seenShorts.insert(i.id);
    jwalk(root, [&](const char* key, const JVal& val) -> bool {
        if (!key) return true;
        for (const char* s : kSkipKeys)
            if (!strcmp(key, s)) return false;
        if (!strcmp(key, "shortsLockupViewModel") || !strcmp(key, "reelItemRenderer")) {
            Item it;
            if (parse_short(key, val, it) && seenShorts.insert(it.id).second) page.shorts.push_back(std::move(it));
            return false;
        }
        if (is_item_key(key)) {
            Item it;
            if (parse_item(key, val, it) && seen.insert(it.id).second) page.items.push_back(std::move(it));
            return false;
        }
        if (!strcmp(key, "continuationItemRenderer")) {
            std::string t = continuation_token(val);
            if (!t.empty()) page.continuation = t;  // last one wins: page-level token comes last
            return false;
        }
        return true;
    });
}

static void check_alerts(const JVal& root, std::string& err) {
    if (const JVal* e = jget(root, "error")) {
        err = jstr(jget(*e, "message"));
        if (err.empty()) err = "YouTube returned an error";
    }
}

Page parse_search(const JVal& root) {
    Page p;
    check_alerts(root, p.error);
    if (const JVal* c = jget(root, "contents")) collect_items(*c, p);
    if (const JVal* c = jget(root, "onResponseReceivedCommands")) collect_items(*c, p);
    return p;
}

static void collect_browse(const JVal& root, Page& p) {
    if (const JVal* c = jget(root, "contents")) collect_items(*c, p);
    if (const JVal* c = jget(root, "onResponseReceivedActions")) collect_items(*c, p);
    if (const JVal* c = jget(root, "onResponseReceivedEndpoints")) collect_items(*c, p);
}

ChannelPage parse_channel(const JVal& root) {
    ChannelPage p;
    check_alerts(root, p.error);
    if (const JVal* m = jpath(root, "metadata.channelMetadataRenderer")) {
        p.id = jstr(jget(*m, "externalId"));
        p.title = jstr(jget(*m, "title"));
        p.description = jstr(jget(*m, "description"));
        p.avatar = pick_thumb(jget(*m, "avatar"), 60);
    }
    if (const JVal* h = jget(root, "header")) {
        if (const JVal* rows = jfind(*h, "metadataRows", 16)) {
            if (rows->IsArray()) {
                for (auto& r : rows->GetArray()) {
                    if (const JVal* parts = jget(r, "metadataParts")) {
                        if (!parts->IsArray()) continue;
                        for (auto& part : parts->GetArray()) {
                            std::string t = jtext(jget(part, "text"));
                            if (contains(t, "subscriber")) p.subscribers = t;
                        }
                    }
                }
            }
        }
        if (p.subscribers.empty()) p.subscribers = jtext(jfind(*h, "subscriberCountText", 12));
        if (p.title.empty()) p.title = jstr(jfind(*h, "pageTitle", 6));
    }
    collect_browse(root, p);
    return p;
}

PlaylistPage parse_playlist(const JVal& root) {
    PlaylistPage p;
    check_alerts(root, p.error);
    if (const JVal* m = jpath(root, "metadata.playlistMetadataRenderer")) {
        p.title = jstr(jget(*m, "title"));
        p.description = jstr(jget(*m, "description"));
    }
    if (const JVal* h = jpath(root, "header.playlistHeaderRenderer")) {
        p.id = jstr(jget(*h, "playlistId"));
        if (p.title.empty()) p.title = jtext(jget(*h, "title"));
        p.author = jtext(jget(*h, "ownerText"));
        p.count = jtext(jget(*h, "numVideosText"));
        if (p.count.empty()) {
            if (const JVal* stats = jget(*h, "stats")) {
                if (stats->IsArray() && !stats->Empty()) p.count = jtext(&(*stats)[0]);
            }
        }
        if (p.count.empty()) p.count = jtext(jget(*h, "byline"));
    } else if (const JVal* h = jget(root, "header")) {
        if (p.title.empty()) p.title = jstr(jfind(*h, "pageTitle", 6));
        if (const JVal* rows = jfind(*h, "metadataRows", 16)) {
            if (rows->IsArray())
                for (auto& r : rows->GetArray()) {
                    std::string line;
                    if (const JVal* parts = jget(r, "metadataParts")) {
                        if (!parts->IsArray()) continue;
                        for (auto& part : parts->GetArray()) {
                            std::string t = jtext(jget(part, "text"));
                            if (starts_with(t, "by ")) t = t.substr(3);
                            // Type / visibility labels sit in the same rows as the owner's name.
                            bool label = t == "Playlist" || t == "Mix" || t == "Podcast" || t == "Public" ||
                                         t == "Private" || t == "Unlisted" || t == "Course";
                            if (contains(t, "video")) p.count = t;
                            else if (p.author.empty() && !t.empty() && !label && !contains(t, "view")) p.author = t;
                        }
                    }
                }
        }
    }
    collect_browse(root, p);
    return p;
}

Page parse_browse_feed(const JVal& root) {
    Page p;
    check_alerts(root, p.error);
    collect_browse(root, p);
    // Topic channels mix in playlists and channels; Home shows videos only.
    p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                 [](const Item& i) { return i.type != ItemType::Video || i.live; }),
                  p.items.end());
    p.continuation.clear();
    return p;
}

static void split_codecs(const std::string& mime, std::string& base, std::string& codecs) {
    size_t semi = mime.find(';');
    base = trim(mime.substr(0, semi));
    codecs.clear();
    size_t c = mime.find("codecs=\"");
    if (c != std::string::npos) {
        size_t e = mime.find('"', c + 8);
        codecs = mime.substr(c + 8, e == std::string::npos ? std::string::npos : e - c - 8);
    }
}

PlayerInfo parse_player(const JVal& root) {
    PlayerInfo info;
    info.status = jstr(root, "playabilityStatus.status");
    info.reason = jstr(root, "playabilityStatus.reason");
    if (info.reason.empty()) info.reason = jtext(jpath(root, "playabilityStatus.errorScreen.playerErrorMessageRenderer.reason"));
    if (const JVal* e = jget(root, "error")) {
        if (info.status.empty()) info.status = "ERROR";
        if (info.reason.empty()) info.reason = jstr(jget(*e, "message"));
    }
    if (const JVal* d = jget(root, "videoDetails")) {
        info.videoId = jstr(jget(*d, "videoId"));
        info.title = jstr(jget(*d, "title"));
        info.author = jstr(jget(*d, "author"));
        info.channelId = jstr(jget(*d, "channelId"));
        info.description = jstr(jget(*d, "shortDescription"));
        info.lengthSeconds = (int)jint(jget(*d, "lengthSeconds"), 0);
        info.viewCount = jint(jget(*d, "viewCount"), -1);
        info.live = jbool(*d, "isLive") || (jbool(*d, "isLiveContent") && info.lengthSeconds == 0);
    }
    if (const JVal* fmts = jpath(root, "streamingData.adaptiveFormats")) {
        if (fmts->IsArray()) {
            for (auto& f : fmts->GetArray()) {
                Format x;
                x.itag = (int)jint(jget(f, "itag"));
                x.url = jstr(jget(f, "url"));
                std::string mime = jstr(jget(f, "mimeType"));
                split_codecs(mime, x.mime, x.codecs);
                x.width = (int)jint(jget(f, "width"));
                x.height = (int)jint(jget(f, "height"));
                x.fps = (int)jint(jget(f, "fps"));
                x.bitrate = jint(jget(f, "bitrate"));
                x.contentLength = jint(jget(f, "contentLength"));
                x.approxDurationMs = jint(jget(f, "approxDurationMs"));
                x.initStart = jint(f, "initRange.start", -1);
                x.initEnd = jint(f, "initRange.end", -1);
                x.indexStart = jint(f, "indexRange.start", -1);
                x.indexEnd = jint(f, "indexRange.end", -1);
                x.sampleRate = (int)jint(jget(f, "audioSampleRate"));
                x.channels = (int)jint(jget(f, "audioChannels"));
                x.drc = jbool(f, "isDrc");
                if (const JVal* at = jget(f, "audioTrack")) x.audioDefault = jbool(*at, "audioIsDefault", true);
                if (x.url.empty()) continue;  // ciphered formats are unusable without JS
                info.formats.push_back(std::move(x));
            }
        }
    }
    if (const JVal* tracks = jpath(root, "captions.playerCaptionsTracklistRenderer.captionTracks")) {
        if (tracks->IsArray()) {
            for (auto& t : tracks->GetArray()) {
                CaptionTrack c;
                c.url = jstr(jget(t, "baseUrl"));
                c.name = jtext(jget(t, "name"));
                c.lang = jstr(jget(t, "languageCode"));
                c.asr = jstr(jget(t, "kind")) == "asr" || starts_with(jstr(jget(t, "vssId")), "a.");
                if (!c.url.empty()) info.captions.push_back(std::move(c));
            }
        }
    }
    return info;
}

WatchInfo parse_next(const JVal& root) {
    WatchInfo w;
    check_alerts(root, w.error);
    Page rel;
    if (const JVal* sec = jpath(root, "contents.twoColumnWatchNextResults.secondaryResults")) collect_items(*sec, rel);
    if (const JVal* sec = jpath(root, "contents.singleColumnWatchNextResults.results.results")) {
        // MWEB layout: related videos live in itemSectionRenderers after the metadata.
        collect_items(*sec, rel);
    }
    if (const JVal* cont = jget(root, "onResponseReceivedEndpoints")) collect_items(*cont, rel);
    w.related = std::move(rel.items);
    w.relatedContinuation = rel.continuation;

    if (const JVal* list = jpath(root, "contents.twoColumnWatchNextResults.results.results.contents")) {
        if (list->IsArray()) {
            for (auto& c : list->GetArray()) {
                if (const JVal* pri = jget(c, "videoPrimaryInfoRenderer")) {
                    w.dateText = jtext(jget(*pri, "dateText"));
                    w.viewsText = jtext(jpath(*pri, "viewCount.videoViewCountRenderer.shortViewCount"));
                    if (w.viewsText.empty())
                        w.viewsText = jtext(jpath(*pri, "viewCount.videoViewCountRenderer.viewCount"));
                    if (const JVal* lb = jfind(*pri, "likeButtonViewModel", 16)) {
                        if (const JVal* bv = jfind(*lb, "buttonViewModel", 12)) w.likes = jstr(jget(*bv, "title"));
                    }
                    if (w.likes == "Like") w.likes.clear();
                }
                if (const JVal* secd = jget(c, "videoSecondaryInfoRenderer")) {
                    if (const JVal* o = jpath(*secd, "owner.videoOwnerRenderer")) {
                        w.channelName = jtext(jget(*o, "title"));
                        w.channelId = browse_id(jget(*o, "navigationEndpoint"));
                        if (w.channelId.empty()) w.channelId = browse_id(jget(*o, "title"));
                        w.subscribers = jtext(jget(*o, "subscriberCountText"));
                        w.channelAvatar = pick_thumb(jget(*o, "thumbnail"), 40);
                        w.verified = has_verified_badge(jget(*o, "badges"));
                    }
                }
                if (const JVal* is = jget(c, "itemSectionRenderer")) {
                    if (jstr(jget(*is, "sectionIdentifier")) == "comment-item-section") {
                        if (const JVal* cir = jfind(*is, "continuationItemRenderer", 10))
                            w.commentsContinuation = continuation_token(*cir);
                    }
                }
            }
        }
    }
    if (const JVal* panels = jget(root, "engagementPanels")) {
        if (panels->IsArray()) {
            for (auto& p : panels->GetArray()) {
                const JVal* ep = jget(p, "engagementPanelSectionListRenderer");
                if (!ep) continue;
                std::string id = jstr(jget(*ep, "panelIdentifier"));
                if (!contains(id, "comment")) continue;
                if (w.commentsContinuation.empty()) {
                    if (const JVal* cir = jfind(*ep, "continuationItemRenderer", 16))
                        w.commentsContinuation = continuation_token(*cir);
                }
                if (w.commentCount.empty()) {
                    if (const JVal* ct = jfind(*ep, "contextualInfo", 12)) w.commentCount = jtext(ct);
                }
                if (w.commentSorts.empty()) {
                    const JVal* items = nullptr;
                    if (const JVal* sm = jfind(*ep, "sortFilterSubMenuRenderer", 10)) items = jget(*sm, "subMenuItems");
                    if (items && items->IsArray()) {
                        for (auto& si : items->GetArray()) {
                            SortOption o;
                            o.title = jstr(jget(si, "title"));
                            if (const JVal* se = jget(si, "serviceEndpoint")) o.token = continuation_token(*se);
                            if (!o.title.empty() && !o.token.empty()) w.commentSorts.push_back(o);
                        }
                    }
                }
            }
        }
    }
    // Chapters from the player bar's markers (creator chapters, else auto-generated ones).
    const JVal* po = jget(root, "playerOverlays");
    if (const JVal* mm = po ? jfind(*po, "markersMap", 10) : nullptr) {
        if (mm->IsArray()) {
            for (auto& m : mm->GetArray()) {
                std::string key = jstr(jget(m, "key"));
                if (key != "DESCRIPTION_CHAPTERS" && key != "AUTO_CHAPTERS") continue;
                const JVal* chs = jpath(m, "value.chapters");
                if (!chs || !chs->IsArray()) continue;
                std::vector<Chapter> list;
                for (auto& c : chs->GetArray()) {
                    const JVal* cr = jget(c, "chapterRenderer");
                    if (!cr) continue;
                    Chapter ch;
                    ch.title = jtext(jget(*cr, "title"));
                    ch.startMs = (int)jint(jget(*cr, "timeRangeStartMillis"), 0);
                    list.push_back(ch);
                }
                if (list.size() >= 2 && (w.chapters.empty() || key == "DESCRIPTION_CHAPTERS")) w.chapters = list;
            }
        }
    }
    return w;
}

CommentsPage parse_comments(const JVal& root) {
    CommentsPage page;
    check_alerts(root, page.error);
    // New format: threads reference entity payloads in frameworkUpdates by key.
    struct Key {
        std::string key, repliesToken;
        bool pinned = false, reply = false;
    };
    std::vector<Key> keys;
    std::vector<Comment> legacy;
    const JVal* eps = jget(root, "onResponseReceivedEndpoints");
    if (eps && eps->IsArray()) {
        for (auto& ep : eps->GetArray()) {
            const JVal* items = jpath(ep, "reloadContinuationItemsCommand.continuationItems");
            if (!items) items = jpath(ep, "appendContinuationItemsAction.continuationItems");
            if (!items || !items->IsArray()) continue;
            for (auto& it : items->GetArray()) {
                if (const JVal* th = jget(it, "commentThreadRenderer")) {
                    std::string rtok;
                    if (const JVal* rep = jpath(*th, "replies.commentRepliesRenderer")) {
                        if (const JVal* cir = jfind(*rep, "continuationItemRenderer", 6)) rtok = continuation_token(*cir);
                    }
                    if (const JVal* cvm = jpath(*th, "commentViewModel.commentViewModel")) {
                        Key k;
                        k.key = jstr(jget(*cvm, "commentKey"));
                        k.pinned = jget(*cvm, "pinnedText") != nullptr;
                        k.repliesToken = rtok;
                        keys.push_back(k);
                    } else if (const JVal* cr = jpath(*th, "comment.commentRenderer")) {
                        Comment c;
                        c.author = jtext(jget(*cr, "authorText"));
                        c.text = jtext(jget(*cr, "contentText"));
                        c.published = jtext(jget(*cr, "publishedTimeText"));
                        c.likes = jtext(jget(*cr, "voteCount"));
                        c.replies = jstr(jget(*cr, "replyCount"));
                        c.pinned = jget(*cr, "pinnedCommentBadge") != nullptr;
                        c.avatar = pick_thumb(jget(*cr, "authorThumbnail"), 40);
                        c.repliesToken = rtok;
                        legacy.push_back(std::move(c));
                    }
                } else if (const JVal* cvm = jget(it, "commentViewModel")) {
                    // Reply pages list bare view models.
                    Key k;
                    k.key = jstr(jget(*cvm, "commentKey"));
                    k.reply = true;
                    keys.push_back(k);
                } else if (const JVal* cir = jget(it, "continuationItemRenderer")) {
                    page.continuation = continuation_token(*cir);
                }
            }
        }
    }
    if (!keys.empty()) {
        std::vector<const JVal*> payloads;
        if (const JVal* muts = jpath(root, "frameworkUpdates.entityBatchUpdate.mutations")) {
            if (muts->IsArray())
                for (auto& m : muts->GetArray())
                    if (const JVal* p = jpath(m, "payload.commentEntityPayload")) payloads.push_back(p);
        }
        for (auto& k : keys) {
            for (const JVal* p : payloads) {
                if (jstr(jget(*p, "key")) != k.key) continue;
                Comment c;
                c.author = jstr(*p, "author.displayName");
                c.creator = jbool(*p, "author.isCreator");
                c.text = jstr(*p, "properties.content.content");
                c.published = jstr(*p, "properties.publishedTime");
                c.likes = jstr(*p, "toolbar.likeCountNotliked");
                c.replies = jstr(*p, "toolbar.replyCount");
                c.avatar = jstr(*p, "author.avatarThumbnailUrl");
                c.pinned = k.pinned;
                c.reply = k.reply;
                c.repliesToken = k.repliesToken;
                page.comments.push_back(std::move(c));
                break;
            }
        }
    }
    for (auto& c : legacy) page.comments.push_back(std::move(c));
    return page;
}

ReelSeed parse_reel_seed(const JVal& root) {
    ReelSeed r;
    check_alerts(root, r.error);
    r.sequence = jstr(jget(root, "sequenceContinuation"));
    if (const JVal* rw = jpath(root, "replacementEndpoint.reelWatchEndpoint")) r.videoId = jstr(jget(*rw, "videoId"));
    if (r.error.empty() && r.videoId.empty() && r.sequence.empty()) r.error = "No Shorts right now";
    return r;
}

Page parse_reel_sequence(const JVal& root) {
    Page p;
    check_alerts(root, p.error);
    if (const JVal* e = jget(root, "entries")) {
        if (e->IsArray()) {
            for (auto& en : e->GetArray()) {
                const JVal* rw = jfind(en, "reelWatchEndpoint", 6);
                if (!rw) continue;
                Item it;
                it.shorts = true;
                it.id = jstr(jget(*rw, "videoId"));
                if (it.id.empty()) continue;
                it.thumb = pick_thumb(jget(*rw, "thumbnail"), 100);
                p.items.push_back(std::move(it));
            }
        }
    }
    if (const JVal* ce = jget(root, "continuationEndpoint")) p.continuation = continuation_token(*ce);
    return p;
}

static std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (!ent.empty() && ent[0] == '#')
            cp = (uint32_t)strtoul(ent.c_str() + (ent.size() > 1 && ent[1] == 'x' ? 2 : 1), nullptr,
                                   ent.size() > 1 && ent[1] == 'x' ? 16 : 10);
        if (!cp) {
            out += s[i];
            continue;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        i = semi;
    }
    return out;
}

static std::string xml_tag(const std::string& s, size_t from, size_t to, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    size_t a = s.find(open, from);
    if (a == std::string::npos || a >= to) return std::string();
    a += open.size();
    size_t b = s.find(std::string("</") + tag + ">", a);
    if (b == std::string::npos || b > to) return std::string();
    return xml_unescape(s.substr(a, b - a));
}

static std::string xml_attr(const std::string& s, size_t from, size_t to, const char* tag, const char* attr) {
    size_t a = s.find(std::string("<") + tag, from);
    if (a == std::string::npos || a >= to) return std::string();
    size_t end = s.find('>', a);
    std::string key = std::string(attr) + "=\"";
    size_t k = s.find(key, a);
    if (k == std::string::npos || k > end) return std::string();
    k += key.size();
    size_t q = s.find('"', k);
    return xml_unescape(s.substr(k, q - k));
}

std::vector<Item> parse_rss(const std::string& xml) {
    std::vector<Item> out;
    size_t pos = 0;
    while (true) {
        size_t a = xml.find("<entry>", pos);
        if (a == std::string::npos) break;
        size_t b = xml.find("</entry>", a);
        if (b == std::string::npos) break;
        Item it;
        it.type = ItemType::Video;
        it.id = xml_tag(xml, a, b, "yt:videoId");
        it.title = xml_tag(xml, a, b, "title");
        it.channelId = xml_tag(xml, a, b, "yt:channelId");
        it.channel = xml_tag(xml, a, b, "name");
        it.time = parse_iso8601(xml_tag(xml, a, b, "published"));
        std::string views = xml_attr(xml, a, b, "media:statistics", "views");
        if (!views.empty()) it.views = format_views(strtoll(views.c_str(), nullptr, 10));
        if (!it.id.empty()) out.push_back(std::move(it));
        pos = b + 8;
    }
    return out;
}

int quality_of(const Format& f) {
    if (f.width > 0 && f.height > 0) return std::min(f.width, f.height);
    return f.height;
}

bool pick_streams(const PlayerInfo& info, int maxHeight, const std::vector<int>& videoPref,
                  const std::vector<int>& audioPref, Format& video, Format& audio) {
    auto rank = [](const std::vector<int>& pref, int itag) {
        auto it = std::find(pref.begin(), pref.end(), itag);
        return it == pref.end() ? 1000 : (int)(it - pref.begin());
    };
    const Format* bestV = nullptr;
    for (const Format& f : info.formats) {
        if (!f.isVideo() || !f.isAvc() || !f.hasRanges()) continue;
        // The Vita decoder (and Vita3K) accept at most 1280x720 worth of pixels.
        if (f.width * f.height > 1280 * 720 || quality_of(f) > maxHeight) continue;
        if (!bestV) {
            bestV = &f;
            continue;
        }
        bool hfps = f.fps > 30, bfps = bestV->fps > 30;
        if (quality_of(f) != quality_of(*bestV)) {
            if (quality_of(f) > quality_of(*bestV)) bestV = &f;
        } else if (hfps != bfps) {
            if (!hfps) bestV = &f;  // prefer 30fps over 60fps at equal height
        } else if (rank(videoPref, f.itag) < rank(videoPref, bestV->itag)) {
            bestV = &f;
        }
    }
    if (!bestV) {  // nothing under the cap: take the smallest AVC stream
        for (const Format& f : info.formats)
            if (f.isVideo() && f.isAvc() && f.hasRanges() && f.width * f.height <= 1280 * 720 &&
                (!bestV || quality_of(f) < quality_of(*bestV)))
                bestV = &f;
    }
    const Format* bestA = nullptr;
    for (const Format& f : info.formats) {
        if (!f.isAudio() || !f.isAac() || !f.hasRanges() || f.drc) continue;
        if (!bestA) {
            bestA = &f;
            continue;
        }
        if (f.audioDefault != bestA->audioDefault) {
            if (f.audioDefault) bestA = &f;
        } else if (rank(audioPref, f.itag) < rank(audioPref, bestA->itag)) {
            bestA = &f;
        }
    }
    if (!bestV || !bestA) return false;
    video = *bestV;
    audio = *bestA;
    return true;
}

}  // namespace vt
