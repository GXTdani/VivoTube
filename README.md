# VivoTube

An unofficial YouTube client for the PlayStation Vita, written from scratch as homebrew.
It talks to YouTube's InnerTube API straight from the console (no companion server) and plays
videos with the Vita's hardware H.264 and AAC decoders.

VivoTube is not affiliated with, endorsed by, or connected to YouTube or Google.

## Features

- **Home** with topic chips (All, Music, News, Sports, Gaming, Podcasts, Recently uploaded), a
  Shorts shelf, and the latest uploads from your subscriptions. Once you've watched or liked
  something, "All" becomes a personal **For you** feed (see below) that keeps loading as you
  scroll
- **Refresh**: pick the current tab again to jump to the top, and again to reload; or pull a
  feed down past its top
- **Mini player**: leaving the watch page (Back, opening a channel, searching) keeps the video
  playing in a small player in the corner
- **Emoji** in titles, comments and descriptions, drawn in colour with Twemoji
- **Text in every major script**: Arabic, Hebrew, Persian and Urdu (joined and right to left),
  Hindi and the other Indic scripts, Thai, Lao, Khmer, Burmese, Sinhala, Georgian, Armenian,
  Amharic, Greek, Cyrillic, Vietnamese, plus Chinese, Japanese and Korean from the Vita's own
  fonts. Text is shaped with HarfBuzz and laid out with the Unicode bidi algorithm (FriBidi),
  using bundled Noto Sans fonts. Settings lists every language in its own script
- **Shorts**: a full-height vertical player that loops, with swipe or D-pad navigation, like,
  dislike, comments, share and subscribe
- **Search** with chips (All, Shorts, Videos, Channels, Playlists, Recently uploaded, Live) and a
  Filters dialog (upload date, type, duration, 4K / HD / subtitles / live, sort order), recent
  searches and infinite scrolling
- **Watch page**: like / dislike, Subscribe, Share, Save to playlist, chapters (on the seek bar
  and as a list), loop, double-tap to seek 10 s, captions, quality from 144p to 720p, resume
  where you left off, fullscreen, autoplay with an Up next countdown
- **Playback speed** from 0.25x to 2x, with time-stretched audio so voices keep their pitch
- **Queue**: "Play next in queue" and "Add to queue" from any video's menu build an Up next list
  on the watch page, and the mini player moves on to the next queued video by itself
- **Not interested** and **Don't recommend channel** on Home and related videos hide a video or
  a whole channel from your recommendations (undo in Settings > Hidden recommendations)
- **Comments** with Top / Newest sorting and reply threads
- **Guide** (the side menu) with your library and subscriptions; **You** page with history,
  playlists, Watch later and Liked videos
- **Playlists**: local playlists on the memory card, plus your YouTube playlists when signed in
- **Sign in with Google** (optional, see below): your YouTube subscriptions, likes and playlists,
  kept in sync with the app
- **Auto-update**: on launch VivoTube checks this repository's latest release and can install it
  in place, then restarts itself
- Without an account, subscriptions, history, likes, Watch later and playlists are all stored
  locally on the memory card

## Controls

| Where | Button | Action |
|---|---|---|
| Anywhere | D-pad / left stick, touch | Move, scroll, fling |
| Anywhere | × (or ○ on Japanese consoles) | Open / select |
| Anywhere | ○ | Back |
| Anywhere | △ | Search |
| Anywhere | SELECT | Open the Guide |
| Lists | □ | More options (queue, Save, Share, go to channel, subscribe, Not interested) |
| Home / Search / Channel | L / R | Switch chip or tab |
| Shorts | Up / Down or L / R, swipe | Previous / next Short |
| Shorts | Right | Move to the like / comment / share buttons |
| Watch | × or START | Play / pause |
| Watch | L / R, double-tap the sides | Seek back / forward 10 s |
| Watch | Right / Left | Move between the player and the side panel |
| Watch | □ | Player settings (quality, speed, captions, loop, chapters) |
| Watch | SELECT | Fullscreen |
| Watch | ○ | Shrink into the mini player (keeps playing) |
| Watch | Down, then × on the channel name | Open the channel (the video keeps playing small) |
| Mini player | Down from the last tab in the rail, or touch | Focus the mini player |
| Mini player | × / □ / START | Expand / close / play or pause (START works from any page) |
| Tabs | × on the current tab | Scroll to the top; again to refresh |

## Installing

1. Download `VivoTube.vpk` from the [latest release](https://github.com/hamburger101/VivoTube/releases/latest).
2. Install it with VitaShell on a hacked Vita, or with *File > Install .vpk* in Vita3K.
3. Launch **VivoTube** from the LiveArea.

Later versions install themselves: when a new release is published here, VivoTube offers it at
launch (*Update now*, *Later* or *Skip this version*). You can also use *Settings > Check for
updates now*, or turn the check off in Settings. The updater downloads the release's `.vpk`,
checks every file's CRC before replacing anything, and restarts the app. If it can't install
in place, it saves the package to `ux0:data/VivoTube/` for you to install with VitaShell.

Data lives in `ux0:data/VivoTube/`:

| File | Contents |
|---|---|
| `subscriptions.json`, `history.json`, `watch_later.json`, `playlists.json`, `ratings.json`, `searches.json`, `settings.json` | Your local library and settings |
| `google_client.json` (optional) | Your own Google OAuth client, used instead of the built-in one |
| `account.json` | The signed-in account's refresh token. Delete it (or sign out) to forget the account |
| `log.txt` | Diagnostic log, useful for bug reports |
| `clients.json` (optional) | Overrides the built-in InnerTube client definitions (see below) |

## Signing in with Google

Signing in is optional. When you're signed in, VivoTube uses the official YouTube Data API to
read and change your subscriptions, likes and playlists. Tokens go only between the Vita and
Google. See the [privacy policy](https://hamburger101.github.io/VivoTube/privacy.html).

1. In VivoTube, choose **Sign in** (top right). The app shows a code.
2. On your phone or PC, open [google.com/device](https://www.google.com/device), enter the code
   and sign in with your Google account.
3. Google hasn't verified VivoTube, so it shows a warning first: choose **Advanced**, then
   **Go to … (unsafe)**, and allow access.

Google limits apps it hasn't verified to 100 accounts in total, and all VivoTube users share one
daily Data API quota. If the built-in sign-in stops working because of either, you can use your
own OAuth client instead.

### Using your own OAuth client

A `google_client.json` on the memory card takes the place of the built-in client.

1. Go to [console.cloud.google.com](https://console.cloud.google.com), create a project, and
   enable **YouTube Data API v3** for it (*APIs & Services > Library*).
2. Set up the **OAuth consent screen** (*Google Auth Platform* or *APIs & Services > OAuth
   consent screen*): user type **External**, any app name, your email as the contact. Under
   *Audience* (or *Test users*), add the Google account you use for YouTube as a test user.
3. Under *Clients* (or *Credentials > Create credentials > OAuth client ID*), create a client of
   type **TVs and Limited Input devices** and download its JSON file.
4. Rename the file to `google_client.json` and copy it to `ux0:data/VivoTube/` (VitaShell's FTP
   or USB mode). In Vita3K, that folder is in the emulator's preferences folder, under
   `ux0/data/VivoTube/`.
5. In VivoTube, choose **Sign in** (top right). The app shows a code: open
   [google.com/device](https://www.google.com/device) on your phone or PC, enter the code, and
   allow access.

### "For you"

YouTube only personalises its own Home feed for its own apps, and it turns away tokens from a
client like this one (VivoTube still tries once per session, in case that changes). So VivoTube
builds the feed itself from what it knows about you: videos related to what you recently watched
and liked (your YouTube likes when signed in, plus the history and likes on the memory card),
with new uploads from your subscriptions mixed in. Videos you finished or disliked are left out.
Refreshing starts from different videos, and scrolling down keeps finding more.

Notes:

- With your own client: while its consent screen's publishing status is *Testing*, Google signs
  you out after 7 days. Choose *Publish app* under *Audience* to stop that, or sign in again now
  and then. Your client also gets its own Data API quota (10,000 units a day), plenty for one
  person.
- Videos still play through InnerTube, signed in or not. Sign-in only changes your library.

If you build VivoTube yourself, your build has no built-in client, so sign-in needs a
`google_client.json`. To compile a client in instead, copy `secrets.cmake.example` to
`secrets.cmake` and fill it in. That file is ignored by git.

### When YouTube changes something

YouTube changes its private API often. The client names, versions and user agents VivoTube
sends live in `assets/clients.json`. To fix a break without rebuilding, copy that file to
`ux0:data/VivoTube/clients.json`, edit it, and restart the app. `tools/probe.py` checks
whether the current definitions still work:

```bash
python -I tools/probe.py
```

## Building

You need [VitaSDK](https://vitasdk.org) plus these packages:

```bash
vdpm curl-mbedtls mbedtls zlib zstd bzip2 libvita2d freetype libpng libjpeg-turbo harfbuzz fribidi
```

Then:

```bash
sh tools/build_vita.sh
```

On Windows, run the script through an MSYS2 bash so the SDK paths resolve (VitaSDK at
`C:\vitasdk`, cmake and make from MSYS2/devkitPro):

```bash
C:/devkitPro/msys2/usr/bin/bash.exe -l tools/build_vita.sh
```

The result is `build/VivoTube.vpk`.

### Releasing

Bump `VITA_VERSION` and `VT_VERSION` in `CMakeLists.txt`, then run `sh tools/release.sh`. It
builds the VPK and publishes it as a GitHub release tagged `v<VT_VERSION>` with the GitHub CLI
(`gh`). Installed copies pick it up the next time they start. The updater looks for a `.vpk`
asset on the repository's latest release, so keep the VPK attached to each release.

### Host tests

The parsers, fMP4 demuxer, captions, store, update, emoji, "For you", text and audio time-stretch
helpers are platform-neutral and tested on the PC (against captured responses in
`tests/fixtures/` where they need them), using zig's bundled clang:

```bash
python -I tools/probe.py --save
sh tests/host/run.sh
```

The captured responses aren't checked in (they contain other people's content and your IP
address), so capture them first with `probe.py --save`. Tests whose fixture is missing are
skipped.

### Regenerating the emoji pack

Download a Twemoji release from [jdecked/twemoji](https://github.com/jdecked/twemoji), then
point the packer at its 72×72 PNGs (needs Pillow):

```bash
python -I tools/make_emoji.py <twemoji>/assets/72x72 --out assets/emoji/emoji.pak
```

### Regenerating the Unicode tables

`src/core/text_tables.inc` (combining marks and styled letters) is generated from Python's
`unicodedata`:

```bash
python -I tools/gen_text_tables.py
```

### Regenerating the LiveArea art

```bash
python -I tools/make_art.py
```

## How playback works

`/youtubei/v1/player` is called as the visionOS client, which currently returns plain DASH
URLs (H.264 up to 1080p, AAC) without a PO token or signature deciphering. VivoTube picks the
best H.264 stream the Vita's decoder accepts (at most 1280×720) and an AAC track. It reads
each stream's `sidx` index and fetches fragments by HTTP byte range, about 30 seconds ahead.
Its own fMP4 demuxer feeds `sceAvcdec` (output into YUV textures drawn by vita2d) and
`sceAudiodec`, and the audio clock drives A/V sync.

## Known limitations

- So far tested only in the Vita3K emulator. Real hardware may need fixes for memory or
  alignment quirks.
- Live streams, premieres, and age-restricted or "made for kids" videos don't play.
- Share shows the link on screen (the Vita has no share sheet).
- Without signing in, the subscriptions feed is built from each channel's public RSS feed
  (latest 15 uploads per channel).
- YouTube removed its Trending page in 2025, so Home uses topic feeds instead.
- A few rare scripts (Tibetan, Mongolian, Syriac, N'Ko and others) have no bundled font and
  show as boxes.
- Skin-tone variants of emoji show in the default tone, to keep the app small.
- If YouTube starts requiring PO tokens for the visionOS client, playback will stop until the
  client definitions are updated (see `clients.json` above).

## License

MIT, see [LICENSE](LICENSE). Bundled third-party components:

- [Roboto](https://fonts.google.com/specimen/Roboto) (Apache License 2.0), in
  `assets/fonts/` with its license
- [Noto Sans](https://github.com/notofonts) fonts for 20 scripts, Copyright 2022 The Noto
  Project Authors (SIL Open Font License 1.1), in `assets/fonts/noto/` with the license
- [Twemoji](https://github.com/jdecked/twemoji) graphics, Copyright Twitter, Inc and other
  contributors (CC-BY 4.0), resized to 32 px and packed by `tools/make_emoji.py` into
  `assets/emoji/emoji.pak`, with the license in `assets/emoji/`
- [RapidJSON](https://rapidjson.org) (MIT), in `third_party/rapidjson/`
- Mozilla's CA certificate bundle (MPL 2.0), as `assets/cacert.pem`
- Linked from VitaSDK packages: libcurl, mbedTLS, zlib, zstd, libvita2d, libjpeg-turbo,
  FreeType, libpng, [HarfBuzz](https://harfbuzz.github.io) (MIT-style "Old MIT" license), under
  their own licenses
- [GNU FriBidi](https://github.com/fribidi/fribidi) (LGPL 2.1 or later), linked statically from
  the VitaSDK package. VivoTube's complete source and build scripts are in this repository, so
  you can rebuild it against a modified FriBidi
