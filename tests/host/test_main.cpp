// Host-side tests for the platform-neutral core. Build and run with tests/host/run.sh.
// Most fixtures are captured live by `python -I tools/probe.py --save`. They aren't in the
// repository (they hold other people's content and the capturing machine's IP address), so a
// test whose fixture is missing is skipped rather than failed.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/core/captions.h"
#include "../../src/core/fmp4.h"
#include "../../src/core/innertube.h"
#include "../../src/core/json_util.h"
#include "../../src/core/parse.h"
#include "../../src/core/store.h"
#include "../../src/core/google.h"
#include "../../src/core/update.h"
#include "../../src/core/zip.h"
#include "../../src/core/util.h"

using namespace vt;

static int g_fail = 0, g_pass = 0, g_skip = 0;
static std::string g_dir = "tests/fixtures/";

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static JDocPtr load(const char* name) {
    std::string text, err;
    if (!read_file(g_dir + name, text)) {
        printf("  skipped: no fixture %s (python -I tools/probe.py --save)\n", name);
        g_skip++;
        return nullptr;
    }
    JDocPtr d = jparse(text, &err);
    if (!d) fprintf(stderr, "  %s: %s\n", name, err.c_str());
    return d;
}

static std::string load_bin(const char* name) {
    std::string s;
    if (!read_file(g_dir + name, s)) {
        printf("  skipped: no fixture %s (python -I tools/probe.py --save)\n", name);
        g_skip++;
    }
    return s;
}

static void check_items(const std::vector<Item>& items, size_t minCount, bool wantChannel) {
    CHECK(items.size() >= minCount);
    size_t withChannel = 0;
    for (const Item& i : items) {
        CHECK(!i.id.empty());
        CHECK(!i.title.empty());
        if (!i.channel.empty()) withChannel++;
    }
    if (wantChannel) CHECK(withChannel * 2 >= items.size());
}

static void test_util() {
    printf("util\n");
    CHECK(parse_duration("12:45") == 765);
    CHECK(parse_duration("1:02:03") == 3723);
    CHECK(parse_duration("abc") == -1);
    CHECK(format_duration(3723) == "1:02:03");
    CHECK(format_duration(65) == "1:05");
    CHECK(parse_relative_age("2 weeks ago") == 2 * 604800);
    CHECK(parse_relative_age("Streamed 3 days ago") == 3 * 86400);
    CHECK(parse_relative_age("2d ago") == 2 * 86400);
    CHECK(parse_relative_age("1.5M") == -1);
    CHECK(parse_iso8601("1970-01-02T00:00:00+00:00") == 86400);
    CHECK(parse_iso8601("2026-10-23T16:00:36+00:00") == 1792771236LL);
    CHECK(parse_iso8601("2026-10-23T18:00:36+02:00") == 1792771236LL);
    CHECK(format_age(1000, 1000 + 3 * 86400 + 5) == "3 days ago");
    CHECK(format_views(448014458) == "448.0M views");
    std::string s = "a\xC3\xA9z";  // aéz
    CHECK(utf8_next(s, 0) == 1);
    CHECK(utf8_next(s, 1) == 3);
    CHECK(utf8_decode(s, 1) == 0xE9);
    CHECK(url_encode("a b&c") == "a%20b%26c");
}

static void test_search() {
    printf("search\n");
    if (JDocPtr d = load("search_web.json")) {
        Page p = parse_search(*d);
        check_items(p.items, 10, true);
        CHECK(!p.continuation.empty());
        size_t withDur = 0;
        for (const Item& i : p.items)
            if (i.type == ItemType::Video && !i.duration.empty()) withDur++;
        CHECK(withDur >= 8);
        printf("  web: %u items, first: %s | %s | %s | %s\n", (unsigned)p.items.size(), p.items[0].title.c_str(),
               p.items[0].channel.c_str(), p.items[0].duration.c_str(), p.items[0].views.c_str());
    }
    if (JDocPtr d = load("search_mweb.json")) {
        Page p = parse_search(*d);
        check_items(p.items, 10, true);
        CHECK(!p.continuation.empty());
    }
    if (JDocPtr d = load("search_web_cont.json")) {
        Page p = parse_search(*d);
        check_items(p.items, 5, true);
        printf("  continuation page: %u items\n", (unsigned)p.items.size());
    }
    if (JDocPtr d = load("search_web.json")) {
        Page p = parse_search(*d);
        size_t snippets = 0, badges = 0, verified = 0;
        for (const Item& i : p.items) {
            if (!i.snippet.empty()) snippets++;
            if (!i.badges.empty()) badges++;
            if (i.verified) verified++;
        }
        CHECK(snippets >= 5);
        CHECK(badges >= 2);
        CHECK(verified >= 1);
        printf("  snippets %u, badges %u, verified %u\n", (unsigned)snippets, (unsigned)badges, (unsigned)verified);
    }
    if (JDocPtr d = load("search_shorts_web.json")) {
        Page p = parse_search(*d);
        CHECK(p.shorts.size() >= 10);
        size_t ok = 0;
        for (const Item& i : p.shorts)
            if (i.shorts && i.id.size() == 11 && !i.title.empty() && !i.thumb.empty()) ok++;
        CHECK(ok == p.shorts.size());
        printf("  shorts filter: %u shorts, %u other items, first: %s | %s\n", (unsigned)p.shorts.size(),
               (unsigned)p.items.size(), p.shorts.empty() ? "" : p.shorts[0].title.c_str(),
               p.shorts.empty() ? "" : p.shorts[0].views.c_str());
    }
}

static void test_reels() {
    printf("reels\n");
    if (JDocPtr d = load("reel_seed_web.json")) {
        ReelSeed r = parse_reel_seed(*d);
        CHECK(r.error.empty());
        CHECK(r.videoId.size() == 11);
        CHECK(!r.sequence.empty());
    }
    if (JDocPtr d = load("reel_seq_web.json")) {
        Page p = parse_reel_sequence(*d);
        CHECK(p.items.size() >= 5);
        CHECK(!p.continuation.empty());
        for (const Item& i : p.items) CHECK(i.shorts && i.id.size() == 11);
        printf("  sequence: %u shorts\n", (unsigned)p.items.size());
    }
}

static void test_player() {
    printf("player\n");
    std::vector<int> vpref{135, 134, 136, 133, 160}, apref{140, 139};
    if (JDocPtr d = load("player_visionos_hd.json")) {
        PlayerInfo info = parse_player(*d);
        CHECK(info.ok());
        CHECK(!info.title.empty());
        CHECK(!info.channelId.empty());
        CHECK(info.lengthSeconds > 0);
        CHECK(!info.captions.empty());
        Format v, a;
        CHECK(pick_streams(info, 480, vpref, apref, v, a));
        CHECK(v.itag == 135);
        CHECK(a.itag == 140);
        CHECK(!a.drc);
        CHECK(v.hasRanges() && a.hasRanges());
        CHECK(pick_streams(info, 720, vpref, apref, v, a) && v.itag == 136);
        CHECK(pick_streams(info, 1080, vpref, apref, v, a) && v.itag == 136);  // 1080p exceeds decoder limit
        CHECK(pick_streams(info, 360, vpref, apref, v, a) && v.itag == 134);
        printf("  %s: %u formats, %u caption tracks\n", info.title.c_str(), (unsigned)info.formats.size(),
               (unsigned)info.captions.size());
    }
    if (JDocPtr d = load("player_visionos.json")) {
        PlayerInfo info = parse_player(*d);
        CHECK(info.ok());
        Format v, a;
        CHECK(pick_streams(info, 480, vpref, apref, v, a));
        CHECK(v.height <= 480);
    }
    const char* bad = "{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in\"}}";
    std::string err;
    JDocPtr d = jparse(bad, &err);
    PlayerInfo info = parse_player(*d);
    CHECK(!info.ok() && info.status == "LOGIN_REQUIRED" && info.reason == "Sign in");
}

static void test_next_and_comments() {
    printf("next / comments\n");
    if (JDocPtr d = load("next_web.json")) {
        WatchInfo w = parse_next(*d);
        check_items(w.related, 10, true);
        CHECK(!w.commentsContinuation.empty());
        CHECK(!w.channelName.empty());
        CHECK(!w.channelId.empty());
        printf("  related %u, channel %s (%s), date %s\n", (unsigned)w.related.size(), w.channelName.c_str(),
               w.subscribers.c_str(), w.dateText.c_str());
    }
    if (JDocPtr d = load("next_chapters_web.json")) {
        WatchInfo w = parse_next(*d);
        CHECK(w.chapters.size() >= 5);
        CHECK(!w.chapters.empty() && w.chapters[0].startMs == 0 && w.chapters[0].title == "Subwoofer Lullaby");
        CHECK(w.chapters.size() >= 2 && w.chapters[1].startMs == 243000);
        CHECK(w.likes == "317K");
        CHECK(w.verified);
        CHECK(w.commentSorts.size() == 2);
        CHECK(w.commentSorts.size() == 2 && w.commentSorts[1].title == "Newest" && !w.commentSorts[1].token.empty());
        printf("  %u chapters, likes %s, sorts %u\n", (unsigned)w.chapters.size(), w.likes.c_str(),
               (unsigned)w.commentSorts.size());
    }
    if (JDocPtr d = load("next_mweb.json")) {
        WatchInfo w = parse_next(*d);
        CHECK(w.related.size() >= 5);
    }
    if (JDocPtr d = load("comments_web.json")) {
        CommentsPage c = parse_comments(*d);
        CHECK(c.comments.size() >= 10);
        CHECK(!c.continuation.empty());
        size_t ok = 0;
        for (const Comment& x : c.comments)
            if (!x.author.empty() && !x.text.empty()) ok++;
        CHECK(ok == c.comments.size());
        size_t withReplies = 0, avatars = 0;
        for (const Comment& x : c.comments) {
            if (!x.repliesToken.empty()) withReplies++;
            if (!x.avatar.empty()) avatars++;
        }
        CHECK(withReplies >= 1);
        CHECK(avatars == c.comments.size());
        printf("  %u comments, first likes=%s replies=%s pinned=%d\n", (unsigned)c.comments.size(),
               c.comments.empty() ? "" : c.comments[0].likes.c_str(),
               c.comments.empty() ? "" : c.comments[0].replies.c_str(), c.comments.empty() ? 0 : c.comments[0].pinned);
    }
}

static void test_replies() {
    printf("replies\n");
    if (JDocPtr d = load("replies_web.json")) {
        CommentsPage c = parse_comments(*d);
        CHECK(c.comments.size() >= 5);
        size_t ok = 0;
        for (const Comment& x : c.comments)
            if (x.reply && !x.author.empty() && !x.text.empty()) ok++;
        CHECK(ok == c.comments.size());
        CHECK(!c.continuation.empty());
        printf("  %u replies\n", (unsigned)c.comments.size());
    }
}

static void test_vertical_streams() {
    printf("vertical streams\n");
    PlayerInfo info;
    info.status = "OK";
    auto add = [&](int itag, int w, int h, const char* mime, const char* codecs) {
        Format f;
        f.itag = itag;
        f.width = w;
        f.height = h;
        f.mime = mime;
        f.codecs = codecs;
        f.initStart = 0;
        f.initEnd = 700;
        f.indexStart = 701;
        f.indexEnd = 1400;
        info.formats.push_back(f);
    };
    add(134, 360, 640, "video/mp4", "avc1.4d401e");
    add(135, 480, 854, "video/mp4", "avc1.4d401f");
    add(136, 720, 1280, "video/mp4", "avc1.4d401f");
    add(140, 0, 0, "audio/mp4", "mp4a.40.2");
    Format v, a;
    std::vector<int> vpref{135, 134, 136}, apref{140};
    CHECK(pick_streams(info, 480, vpref, apref, v, a) && v.itag == 135);  // vertical 480p is not skipped
    CHECK(pick_streams(info, 720, vpref, apref, v, a) && v.itag == 136);
    CHECK(quality_of(v) == 720);
}

static void test_search_params() {
    printf("search params\n");
    SearchFilters f;
    CHECK(search_params(f).empty());
    f.type = 1;
    CHECK(search_params(f) == "EgIQAQ%3D%3D");
    f.type = 9;
    CHECK(search_params(f) == "EgIQCQ%3D%3D");
    f = SearchFilters();
    f.upload = 2;
    CHECK(search_params(f) == "EgIIAg%3D%3D");
    f = SearchFilters();
    f.duration = 4;
    CHECK(search_params(f) == "EgIYBA%3D%3D");
    f = SearchFilters();
    f.live = true;
    CHECK(search_params(f) == "EgJAAQ%3D%3D");
    f = SearchFilters();
    f.uhd = true;
    CHECK(search_params(f) == "EgJwAQ%3D%3D");
    f = SearchFilters();
    f.sort = 3;
    CHECK(search_params(f) == "CAM%3D");
    f.type = 1;
    f.upload = 3;
    CHECK(search_params(f) == "CAMSBAgDEAE%3D");
}

static void test_update() {
    printf("update\n");
    CHECK(compare_versions("1.10.0", "1.9.3") == 1);
    CHECK(compare_versions("v1.1.0", "1.1") == 0);
    CHECK(compare_versions("1.1", "1.1.1") == -1);
    CHECK(compare_versions("2.0-beta", "1.9") == 1);
    CHECK(compare_versions("1.0", "v1.0.0") == 0);
    const char* rel =
        "{\"tag_name\":\"v1.2.0\",\"name\":\"VivoTube 1.2\",\"html_url\":\"https://github.com/x/y/releases/tag/v1.2.0\","
        "\"body\":\"## What's new\\r\\n\\r\\n- **Shorts** tab\\n* Faster `search`\\n\","
        "\"assets\":[{\"name\":\"notes.txt\",\"browser_download_url\":\"https://x/notes.txt\",\"size\":3},"
        "{\"name\":\"VivoTube.vpk\",\"browser_download_url\":\"https://x/VivoTube.vpk\",\"size\":4321}]}";
    std::string err;
    JDocPtr d = jparse(rel, &err);
    CHECK(d != nullptr);
    if (d) {
        Release r = parse_release(*d);
        CHECK(r.ok());
        CHECK(r.version == "1.2.0" && r.tag == "v1.2.0");
        CHECK(r.vpkUrl == "https://x/VivoTube.vpk" && r.vpkSize == 4321);
        CHECK(r.notes == "What's new\n\n\xE2\x80\xA2 Shorts tab\n\xE2\x80\xA2 Faster search");
    }
    JDocPtr nf = jparse("{\"message\":\"Not Found\"}", &err);
    CHECK(nf && !parse_release(*nf).ok() && parse_release(*nf).error == "Not Found");
    JDocPtr noVpk = jparse("{\"tag_name\":\"v2\",\"assets\":[]}", &err);
    CHECK(noVpk && !parse_release(*noVpk).ok());

    std::string zip;
    CHECK(read_file(g_dir + "update_test.zip", zip));
    std::vector<ZipEntry> es;
    CHECK(zip_entries(zip, es, &err));
    CHECK(es.size() == 3);
    if (es.size() == 3) {
        CHECK(es[0].name == "eboot.bin" && es[0].method == 0 && es[0].size == 15);
        CHECK(zip.compare(es[0].dataOffset, 15, "EBOOT-DATA-1234") == 0);
        CHECK(crc32_update(0, (const uint8_t*)zip.data() + es[0].dataOffset, es[0].size) == es[0].crc);
        CHECK(es[1].isDir());
        CHECK(es[2].name == "assets/fonts/readme.txt" && es[2].method == 8 && es[2].size == 29);
    }
    std::string bad;
    CHECK(read_file(g_dir + "update_unsafe.zip", bad));
    CHECK(!zip_entries(bad, es, &err) && contains(err, "unsafe"));
    CHECK(!zip_entries("not a zip at all, definitely not", es, &err));
}

static void test_google() {
    printf("google\n");
    CHECK(iso8601_seconds("PT1H2M3S") == 3723);
    CHECK(iso8601_seconds("PT45S") == 45);
    CHECK(iso8601_seconds("P1DT1M") == 86460);
    std::string err;
    JDocPtr d = jparse("{\"device_code\":\"dc\",\"user_code\":\"ABCD-EFGH\",\"expires_in\":1800,\"interval\":5,"
                       "\"verification_url\":\"https://www.google.com/device\"}", &err);
    CHECK(d != nullptr);
    if (d) {
        DeviceCode c = parse_device_code(*d);
        CHECK(c.error.empty() && c.userCode == "ABCD-EFGH" && c.deviceCode == "dc" && c.interval == 5);
        CHECK(c.verifyUrl == "https://www.google.com/device");
    }
    d = jparse("{\"error\":\"invalid_client\",\"error_description\":\"The OAuth client was not found.\"}", &err);
    CHECK(d && parse_device_code(*d).error == "invalid_client: The OAuth client was not found.");
    d = jparse("{\"error\":\"authorization_pending\"}", &err);
    CHECK(d && parse_token(*d).pending());
    d = jparse("{\"access_token\":\"at\",\"refresh_token\":\"rt\",\"expires_in\":3599}", &err);
    CHECK(d && parse_token(*d).error.empty() && parse_token(*d).refresh == "rt" && parse_token(*d).expiresIn == 3599);

    d = jparse("{\"items\":[{\"id\":\"UCme\",\"snippet\":{\"title\":\"Me\",\"customUrl\":\"@me\","
               "\"thumbnails\":{\"default\":{\"url\":\"https://yt3/a.jpg\"}}}}]}", &err);
    GoogleProfile prof;
    CHECK(d && parse_profile(*d, prof) && prof.channelId == "UCme" && prof.handle == "@me" && prof.avatar == "https://yt3/a.jpg");

    d = jparse("{\"nextPageToken\":\"P2\",\"items\":[{\"snippet\":{\"title\":\"Chan\",\"resourceId\":"
               "{\"kind\":\"youtube#channel\",\"channelId\":\"UCx\"},\"thumbnails\":{\"medium\":{\"url\":\"m\"}}}}]}",
               &err);
    std::vector<Subscription> subs;
    std::string next;
    if (d) parse_api_subscriptions(*d, subs, next);
    CHECK(subs.size() == 1 && subs[0].id == "UCx" && subs[0].name == "Chan" && subs[0].avatar == "m" && next == "P2");

    d = jparse("{\"items\":[{\"id\":\"PL1\",\"snippet\":{\"title\":\"Mix\",\"channelTitle\":\"Me\"},"
               "\"contentDetails\":{\"itemCount\":3}}]}", &err);
    std::vector<Item> pls;
    if (d) parse_api_playlists(*d, pls, next);
    CHECK(pls.size() == 1 && pls[0].type == ItemType::Playlist && pls[0].extra == "3 videos" && next.empty());

    d = jparse("{\"items\":[{\"id\":\"vid12345678\",\"snippet\":{\"title\":\"V\",\"channelTitle\":\"C\","
               "\"channelId\":\"UCc\"},\"contentDetails\":{\"duration\":\"PT4M5S\"},\"statistics\":{\"viewCount\":\"1500\"}},"
               "{\"snippet\":{\"title\":\"Private video\",\"resourceId\":{\"videoId\":\"zzz\"}}},"
               "{\"snippet\":{\"title\":\"P\",\"channelTitle\":\"Owner of list\",\"videoOwnerChannelTitle\":\"Up\","
               "\"videoOwnerChannelId\":\"UCu\",\"resourceId\":{\"videoId\":\"pli12345678\"}}}]}",
               &err);
    std::vector<Item> vids;
    if (d) parse_api_videos(*d, vids, next);
    CHECK(vids.size() == 2);
    if (vids.size() == 2) {
        CHECK(vids[0].id == "vid12345678" && vids[0].duration == "4:05" && vids[0].channel == "C" && !vids[0].views.empty());
        CHECK(vids[1].id == "pli12345678" && vids[1].channel == "Up" && vids[1].channelId == "UCu");
    }
    d = jparse("{\"error\":{\"code\":403,\"message\":\"x\",\"errors\":[{\"reason\":\"quotaExceeded\"}]}}", &err);
    CHECK(d && contains(parse_api_error(*d), "quota"));
    d = jparse("{\"kind\":\"ok\"}", &err);
    CHECK(d && parse_api_error(*d).empty());
}

static void test_browse() {
    printf("browse\n");
    if (JDocPtr d = load("channel_videos_web.json")) {
        ChannelPage p = parse_channel(*d);
        CHECK(!p.title.empty());
        CHECK(starts_with(p.id, "UC"));
        CHECK(!p.items.empty());
        CHECK(!p.avatar.empty());
        printf("  channel %s, %s, %u videos\n", p.title.c_str(), p.subscribers.c_str(), (unsigned)p.items.size());
    }
    if (JDocPtr d = load("channel_playlists_web.json")) {
        ChannelPage p = parse_channel(*d);
        CHECK(!p.items.empty());
        CHECK(!p.items.empty() && p.items[0].type == ItemType::Playlist);
    }
    if (JDocPtr d = load("playlist_web.json")) {
        PlaylistPage p = parse_playlist(*d);
        CHECK(!p.title.empty());
        check_items(p.items, 50, false);
        printf("  playlist %s by %s: %s, %u items\n", p.title.c_str(), p.author.c_str(), p.count.c_str(),
               (unsigned)p.items.size());
    }
    for (const char* f : {"home_UC-9-kyTW8ZkZNDHQJ6FgpwQ.json", "home_UCYfdidRxbB8Qhf0Nx7ioOYw.json",
                          "home_UCEgdi0XIXXZ-qJOFPf4JSKw.json"}) {
        if (JDocPtr d = load(f)) {
            Page p = parse_browse_feed(*d);
            check_items(p.items, 10, true);
            printf("  %s: %u videos\n", f, (unsigned)p.items.size());
        }
    }
    std::string xml = load_bin("rss.xml");
    if (xml.empty()) return;
    std::vector<Item> rss = parse_rss(xml);
    CHECK(rss.size() >= 5);
    for (const Item& i : rss) {
        CHECK(i.time > 1000000000);
        CHECK(!i.title.empty());
        CHECK(starts_with(i.channelId, "UC"));
    }
}

static void test_fmp4() {
    printf("fmp4\n");
    std::string vi = load_bin("video_init_sidx.bin"), vs = load_bin("video_seg0.bin");
    if (vi.empty() || vs.empty()) return;
    Fmp4Init vinit;
    SegmentIndex vidx;
    std::string err;
    CHECK(fmp4_parse_init((const uint8_t*)vi.data(), vi.size(), vinit, &vidx, &err));
    CHECK(vinit.isVideo && !vinit.isAudio);
    CHECK(vinit.width > 0 && vinit.height > 0);
    CHECK(!vinit.sps.empty() && !vinit.pps.empty());
    CHECK(vinit.nalLengthSize == 4);
    CHECK(vinit.timescale > 0);
    CHECK(!vidx.refs.empty());
    CHECK(!vidx.refs.empty() && vidx.refs[0].offset == vi.size());  // first fragment follows the sidx
    printf("  video %dx%d ts=%u segments=%u first=%.2fs\n", vinit.width, vinit.height, vinit.timescale,
           (unsigned)vidx.refs.size(), vidx.refs.empty() ? 0.0 : vidx.durationSeconds(0));
    std::vector<Sample> samples;
    CHECK(fmp4_parse_fragments((const uint8_t*)vs.data(), vs.size(), vinit, samples, &err));
    CHECK(!samples.empty() && samples[0].key);
    std::vector<uint8_t> annexb;
    size_t converted = 0;
    for (size_t i = 0; i < samples.size(); i++) {
        const Sample& s = samples[i];
        if (avcc_to_annexb(vinit, (const uint8_t*)vs.data() + s.offset, s.size, i == 0, annexb)) converted++;
    }
    CHECK(converted == samples.size());
    CHECK(vidx.find(0.0) == 0);
    CHECK(vidx.find(1e9) == vidx.refs.size() - 1);
    printf("  %u video samples in first fragments, keyframes:", (unsigned)samples.size());
    int keys = 0;
    for (auto& s : samples) keys += s.key;
    printf(" %d\n", keys);

    std::string ai = load_bin("audio_init_sidx.bin"), as = load_bin("audio_seg0.bin");
    if (ai.empty() || as.empty()) return;
    Fmp4Init ainit;
    SegmentIndex aidx;
    CHECK(fmp4_parse_init((const uint8_t*)ai.data(), ai.size(), ainit, &aidx, &err));
    CHECK(ainit.isAudio);
    CHECK(ainit.audioObjectType == 2);
    CHECK(ainit.sampleRate == 44100);
    CHECK(ainit.freqIndex == 4);
    CHECK(ainit.channels == 2);
    std::vector<Sample> as2;
    err.clear();
    fmp4_parse_fragments((const uint8_t*)as.data(), as.size(), ainit, as2, &err);  // fixture is truncated
    CHECK(as2.size() > 100);
    uint8_t h[7];
    make_adts_header(ainit, 371, h);
    CHECK(h[0] == 0xFF && h[1] == 0xF1);
    CHECK(((h[2] >> 6) & 3) == 1);        // LC profile
    CHECK(((h[2] >> 2) & 0xF) == 4);      // 44.1 kHz
    int len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
    CHECK(len == 378);
    printf("  audio aot=%d %dHz ch=%d, %u samples, first dur=%u\n", ainit.audioObjectType, ainit.sampleRate,
           ainit.channels, (unsigned)as2.size(), as2.empty() ? 0 : as2[0].duration);
}

static void test_captions() {
    printf("captions\n");
    std::string text;
    CHECK(read_file(g_dir + "captions_synthetic.json", text));
    Captions c;
    std::string err;
    CHECK(c.parse(text, &err));
    CHECK(c.size() == 3);
    CHECK(c.at(0.2).empty());
    CHECK(c.at(1.0) == "Synthetic caption line one");
    CHECK(c.at(2.5) == "line two");  // previous cue is cut at the next cue's start
    CHECK(c.at(7.0).empty());
    CHECK(c.at(9.0) == "third & last");
}

static void test_store() {
    printf("store\n");
    std::string dir = "tests/host/tmp_store";
    make_dirs(dir);
    {
        Store s(dir);
        s.load();
        s.subs.clear();
        s.history.clear();
        s.watchLater.clear();
        s.subscribe("UCabc", "Zed", "");
        s.subscribe("UCdef", "Alpha", "http://x/a.jpg");
        s.subscribe("UCabc", "Zed", "");  // duplicate ignored
        Item v;
        v.id = "vid1";
        v.title = "Title \"quoted\" \xC3\xA9";
        v.channel = "Chan";
        v.duration = "1:00";
        s.recordWatch(v, 30, 60);
        s.toggleWatchLater(v);
        s.addSearch("vita homebrew");
        s.addSearch("  vita homebrew ");
        s.settings.maxHeight = 720;
        s.settings.captions = true;
        s.settings.skipVersion = "1.2.0";
        s.saveSettings();
        s.setRating(v, 1);
        Item w2;
        w2.id = "vid2";
        w2.title = "Other";
        w2.shorts = true;
        s.setRating(w2, -1);
        std::string pid = s.createPlaylist("  Road trip ");
        CHECK(s.togglePlaylistItem(pid, v));
        CHECK(s.togglePlaylistItem(pid, w2));
        CHECK(!s.togglePlaylistItem(pid, v));  // second toggle removes
        std::string pid2 = s.createPlaylist("");
        s.deletePlaylist(pid2);
    }
    Store s(dir);
    s.load();
    CHECK(s.subs.size() == 2);
    CHECK(s.subs.size() == 2 && s.subs[0].name == "Alpha");  // sorted by name
    CHECK(s.isSubscribed("UCabc"));
    CHECK(s.history.size() == 1 && s.history[0].item.title == "Title \"quoted\" \xC3\xA9");
    CHECK(s.resumePosition("vid1") == 30);
    CHECK(s.watchedFraction("vid1") > 0.49f && s.watchedFraction("vid1") < 0.51f);
    CHECK(s.watchedFraction("nope") < 0);
    CHECK(s.inWatchLater("vid1"));
    CHECK(s.searches.size() == 1);
    CHECK(s.settings.maxHeight == 720 && s.settings.captions);
    CHECK(s.settings.skipVersion == "1.2.0" && s.settings.checkUpdates);
    CHECK(s.rating("vid1") == 1 && s.rating("vid2") == -1 && s.rating("x") == 0);
    CHECK(s.liked.size() == 1 && s.liked[0].title == s.history[0].item.title);
    CHECK(s.playlists.size() == 1 && s.playlists[0].title == "Road trip");
    CHECK(s.playlists.size() == 1 && s.playlists[0].items.size() == 1 && s.playlists[0].items[0].id == "vid2" &&
          s.playlists[0].items[0].shorts);
    CHECK(s.playlists.size() == 1 && s.inPlaylist(s.playlists[0].id, "vid2") && !s.inPlaylist(s.playlists[0].id, "vid1"));
    s.recordWatch(s.history[0].item, 58, 60);
    CHECK(s.resumePosition("vid1") == 0);  // near the end counts as finished
    CHECK(s.watchedFraction("vid1") == 1.0f);  // ...and shows a full bar
    s.toggleWatchLater(s.history[0].item);
    CHECK(!s.inWatchLater("vid1"));
    s.unsubscribe("UCabc");
    CHECK(!s.isSubscribed("UCabc"));
}

static void test_innertube_body() {
    printf("innertube body\n");
    InnerTube yt;
    std::string err;
    CHECK(yt.loadProfiles("assets/clients.json", "", &err));
    yt.setLocale("de", "DE");
    std::string body = yt.buildBody("player", "{\"videoId\":\"abc\",\"contentCheckOk\":true}");
    JDocPtr d = jparse(body, &err);
    CHECK(d != nullptr);
    if (d) {
        CHECK(jstr(*d, "context.client.clientName") == "VISIONOS");
        CHECK(jstr(*d, "context.client.hl") == "de");
        CHECK(jstr(*d, "context.client.gl") == "DE");
        CHECK(jstr(*d, "videoId") == "abc");
        CHECK(jbool(*d, "contentCheckOk"));
    }
    body = yt.buildBody("search", "{\"query\":\"x\"}");
    d = jparse(body, &err);
    CHECK(d && jstr(*d, "context.client.clientName") == "WEB");
    CHECK(yt.videoItags().size() >= 3 && yt.videoItags()[0] == 135);
}

int main(int argc, char** argv) {
    if (argc > 1) g_dir = argv[1];
    test_util();
    test_search();
    test_player();
    test_next_and_comments();
    test_replies();
    test_reels();
    test_vertical_streams();
    test_update();
    test_google();
    test_search_params();
    test_browse();
    test_fmp4();
    test_captions();
    test_store();
    test_innertube_body();
    printf("\n%d passed, %d failed", g_pass, g_fail);
    if (g_skip) printf(", %d skipped for missing fixtures", g_skip);
    printf("\n");
    return g_fail ? 1 : 0;
}
