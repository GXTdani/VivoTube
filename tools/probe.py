#!/usr/bin/env python3
"""Probe YouTube's InnerTube API with the same client profiles VivoTube uses.

Checks that each endpoint still answers with the JSON shapes the app parses and
(optionally) saves the raw responses as test fixtures.

    python -I tools/probe.py                 # check only
    python -I tools/probe.py --save          # also write tests/fixtures/*.json
    python -I tools/probe.py --video ID      # use a specific video

Standard library only.
"""
import argparse
import gzip
import json
import os
import re
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIXTURES = os.path.join(ROOT, "tests", "fixtures")

with open(os.path.join(ROOT, "assets", "clients.json"), "r", encoding="utf-8") as f:
    CLIENTS = json.load(f)["clients"]

BASE = "https://www.youtube.com/youtubei/v1/"


def client_context(name, visitor):
    c = dict(CLIENTS[name]["context"])
    c["hl"] = "en"
    c["gl"] = "US"
    if visitor:
        c["visitorData"] = visitor
    return {"client": c}


def headers_for(name, visitor):
    prof = CLIENTS[name]
    h = {
        "Content-Type": "application/json",
        "User-Agent": prof["userAgent"],
        "X-YouTube-Client-Name": str(prof["clientId"]),
        "X-YouTube-Client-Version": prof["context"]["clientVersion"],
        "Accept-Encoding": "gzip",
        "Origin": "https://www.youtube.com",
    }
    for k, v in prof.get("headers", {}).items():
        h[k] = v
    if visitor:
        h["X-Goog-Visitor-Id"] = visitor
    return h


def post(endpoint, client, body, visitor):
    body = dict(body)
    body["context"] = client_context(client, visitor)
    req = urllib.request.Request(
        BASE + endpoint + "?prettyPrint=false",
        data=json.dumps(body).encode(),
        headers=headers_for(client, visitor),
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            raw = r.read()
            if r.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            return r.status, json.loads(raw)
    except urllib.error.HTTPError as e:
        return e.code, None


def get(url, rng=None, ua="Mozilla/5.0"):
    h = {"User-Agent": ua}
    if rng:
        h["Range"] = "bytes=%d-%d" % rng
    req = urllib.request.Request(url, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, b""


def walk(obj, key):
    """Yield every value stored under `key` anywhere in obj."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k == key:
                yield v
            yield from walk(v, key)
    elif isinstance(obj, list):
        for v in obj:
            yield from walk(v, key)


def tracks_of(player):
    return player.get("captions", {}).get("playerCaptionsTracklistRenderer", {}).get("captionTracks", [])


def scrub(o):
    """Replace URLs that embed the caller's IP (stream / onesie URLs) so fixtures are shareable."""
    if isinstance(o, dict):
        return {k: scrub(v) for k, v in o.items()}
    if isinstance(o, list):
        return [scrub(v) for v in o]
    if isinstance(o, str) and ("googlevideo.com" in o or re.search(r"[?&]ip=\d", o)):
        return "https://example.invalid/scrubbed"
    return o


def save(name, data, binary=False):
    os.makedirs(FIXTURES, exist_ok=True)
    if not binary:
        data = scrub(data)
    path = os.path.join(FIXTURES, name)
    if binary:
        with open(path, "wb") as f:
            f.write(data)
    else:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
    print("   saved", os.path.relpath(path, ROOT))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--save", action="store_true")
    ap.add_argument("--video", default="jNQXAC9IVRw")  # "Me at the zoo"
    ap.add_argument("--query", default="vita homebrew")
    a = ap.parse_args()
    ok = True

    def check(label, cond, extra=""):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + label + ("  " + extra if extra else ""))
        ok = ok and bool(cond)

    st, vid = post("visitor_id", "WEB", {}, None)
    visitor = (vid or {}).get("responseContext", {}).get("visitorData")
    check("visitor_id", st == 200 and visitor, "status=%s" % st)

    for client in ("WEB", "MWEB"):
        st, res = post("search", client, {"query": a.query}, visitor)
        n_old = len(list(walk(res, "videoRenderer"))) + len(list(walk(res, "videoWithContextRenderer")))
        n_new = len([x for x in walk(res, "lockupViewModel")])
        cont = [c for r in walk(res, "continuationItemRenderer") for c in walk(r, "continuationCommand")]
        check("search/%s" % client, st == 200 and (n_old + n_new) > 0,
              "videos(old)=%d lockups=%d continuation=%s" % (n_old, n_new, bool(cont)))
        if a.save and res:
            save("search_%s.json" % client.lower(), res)
        if client == "WEB" and cont and res:
            st2, res2 = post("search", client, {"continuation": cont[0]["token"]}, visitor)
            n2 = len(list(walk(res2, "videoRenderer")))
            check("search/WEB continuation", st2 == 200 and n2 > 0, "videos=%d" % n2)
            if a.save and res2:
                save("search_web_cont.json", res2)

    st, pl = post("player", "VISIONOS", {"videoId": a.video, "contentCheckOk": True, "racyCheckOk": True}, visitor)
    status = (pl or {}).get("playabilityStatus", {}).get("status")
    fmts = (pl or {}).get("streamingData", {}).get("adaptiveFormats", [])
    avc = [f for f in fmts if "avc1" in f.get("mimeType", "")]
    aac = [f for f in fmts if "mp4a" in f.get("mimeType", "")]
    check("player/VISIONOS", st == 200 and status == "OK" and avc and aac,
          "status=%s avc=%s aac=%s" % (status, [f["itag"] for f in avc], [f["itag"] for f in aac]))
    tracks = (pl or {}).get("captions", {}).get("playerCaptionsTracklistRenderer", {}).get("captionTracks", [])
    print("   captions:", [t.get("languageCode") for t in tracks])
    if a.save and pl:
        # Stream URLs embed the caller's IP address; scrub them before writing fixtures.
        clean = json.loads(json.dumps(pl))
        sd = clean.get("streamingData", {})
        for f in sd.get("adaptiveFormats", []) + sd.get("formats", []):
            if "url" in f:
                f["url"] = "https://example.invalid/videoplayback?itag=%d" % f["itag"]
        sd.pop("hlsManifestUrl", None)
        sd.pop("serverAbrStreamingUrl", None)
        for t in tracks_of(clean):
            t["baseUrl"] = "https://www.youtube.com/api/timedtext?v=%s&lang=%s" % (a.video, t.get("languageCode", ""))
        clean.pop("playbackTracking", None)
        save("player_visionos.json", clean)

    if avc:
        v = sorted(avc, key=lambda f: f.get("height", 0))[0]
        url = v["url"]
        init_end = int(v["indexRange"]["end"])
        st, data = get(url, (0, init_end), CLIENTS["VISIONOS"]["userAgent"])
        check("stream init+sidx (itag %s)" % v["itag"], st in (200, 206) and data[4:8] == b"ftyp", "status=%s bytes=%d" % (st, len(data)))
        if a.save and data:
            save("video_init_sidx.bin", data, binary=True)
        total = int(v.get("contentLength", "0"))
        st, seg = get(url, (init_end + 1, min(total - 1, init_end + 400000)), CLIENTS["VISIONOS"]["userAgent"])
        check("stream first segment bytes", st in (200, 206) and seg[4:8] == b"moof", "status=%s" % st)
        if a.save and seg:
            save("video_seg0.bin", seg, binary=True)
        mid = total // 2
        st, _ = get(url, (mid, mid + 65535), CLIENTS["VISIONOS"]["userAgent"])
        check("stream mid-file range", st in (200, 206), "status=%s" % st)
    if aac:
        au = [f for f in aac if f["itag"] == 140] or aac
        au = au[0]
        init_end = int(au["indexRange"]["end"])
        st, data = get(au["url"], (0, init_end), CLIENTS["VISIONOS"]["userAgent"])
        if a.save and data:
            save("audio_init_sidx.bin", data, binary=True)
        st, seg = get(au["url"], (init_end + 1, init_end + 120000), CLIENTS["VISIONOS"]["userAgent"])
        check("audio segment bytes", st in (200, 206) and seg[4:8] == b"moof", "status=%s" % st)
        if a.save and seg:
            save("audio_seg0.bin", seg, binary=True)

    if tracks:
        st, cap = get(tracks[0]["baseUrl"] + "&fmt=json3", ua=CLIENTS["VISIONOS"]["userAgent"])
        good = st == 200 and cap[:1] == b"{"
        check("captions json3", good and "events" in json.loads(cap or b"{}"), "status=%s bytes=%d" % (st, len(cap)))
        # Caption text is the uploader's content, so it is checked here but never saved;
        # tests use the hand-written tests/fixtures/captions_synthetic.json instead.

    for client in ("WEB", "MWEB"):
        st, nx = post("next", client, {"videoId": a.video}, visitor)
        rel = len(list(walk(nx, "lockupViewModel"))) + len(list(walk(nx, "compactVideoRenderer"))) + len(list(walk(nx, "videoWithContextRenderer")))
        check("next/%s" % client, st == 200 and rel > 0, "related=%d" % rel)
        if a.save and nx:
            save("next_%s.json" % client.lower(), nx)
        if client == "WEB" and nx:
            token = None
            for sec in walk(nx, "itemSectionRenderer"):
                if sec.get("sectionIdentifier") == "comment-item-section":
                    for c in walk(sec, "continuationCommand"):
                        token = c["token"]
            if not token:
                for p in walk(nx, "engagementPanelSectionListRenderer"):
                    if "comment" in json.dumps(p.get("panelIdentifier", "")):
                        for c in walk(p, "continuationCommand"):
                            token = c["token"]
                            break
            if token:
                st, cm = post("next", client, {"continuation": token}, visitor)
                n_ent = len(list(walk(cm, "commentEntityPayload")))
                n_old = len(list(walk(cm, "commentRenderer")))
                check("comments", st == 200 and (n_ent + n_old) > 0, "entities=%d legacy=%d" % (n_ent, n_old))
                if a.save and cm:
                    save("comments_web.json", cm)
            else:
                check("comments continuation token", False)

    channel = None
    if pl:
        channel = pl.get("videoDetails", {}).get("channelId")
    channel = channel or "UC4QobU6STFB0P71PMvOGN5A"
    for client in ("WEB", "MWEB"):
        st, ch = post("browse", client, {"browseId": channel, "params": "EgZ2aWRlb3PyBgQKAjoA"}, visitor)
        n = len(list(walk(ch, "lockupViewModel"))) + len(list(walk(ch, "videoRenderer"))) + len(list(walk(ch, "videoWithContextRenderer")))
        check("browse channel videos/%s" % client, st == 200 and n > 0, "videos=%d" % n)
        if a.save and ch:
            save("channel_videos_%s.json" % client.lower(), ch)
    st, ch = post("browse", "WEB", {"browseId": channel, "params": "EglwbGF5bGlzdHPyBgQKAkIA"}, visitor)
    n = len(list(walk(ch, "lockupViewModel"))) + len(list(walk(ch, "gridPlaylistRenderer")))
    print("   channel playlists tab: status=%s items=%d" % (st, n))
    if a.save and ch:
        save("channel_playlists_web.json", ch)

    st, pls = post("search", "WEB", {"query": "lofi", "params": "EgIQAw%3D%3D"}, visitor)
    pids = [x.get("contentId") for x in walk(pls, "lockupViewModel") if x.get("contentType") == "LOCKUP_CONTENT_TYPE_PLAYLIST"]
    pids += [x.get("playlistId") for x in walk(pls, "playlistRenderer")]
    pids = [p for p in pids if p]
    if pids:
        st, pb = post("browse", "WEB", {"browseId": "VL" + pids[0]}, visitor)
        n = len(list(walk(pb, "playlistVideoRenderer"))) + len(list(walk(pb, "lockupViewModel")))
        check("browse playlist", st == 200 and n > 0, "videos=%d" % n)
        if a.save and pb:
            save("playlist_web.json", pb)
    else:
        check("playlist search", False)

    for bid in ("FEhype_leaderboard", "FEwhat_to_watch", "UC-9-kyTW8ZkZNDHQJ6FgpwQ", "UCOpNcN46UbXVtpKMrmU4Abg",
                "UCYfdidRxbB8Qhf0Nx7ioOYw", "UCEgdi0XIXXZ-qJOFPf4JSKw"):
        st, hb = post("browse", "WEB", {"browseId": bid}, visitor)
        n = len(list(walk(hb, "lockupViewModel"))) + len(list(walk(hb, "videoRenderer"))) + len(list(walk(hb, "gridVideoRenderer")))
        print("   home candidate %-26s status=%s videos=%d" % (bid, st, n))
        if a.save and hb and n:
            save("home_%s.json" % bid, hb)

    print("\nALL PASS" if ok else "\nSOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
