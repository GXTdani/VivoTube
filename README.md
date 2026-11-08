# VivoTube

An unofficial YouTube client for the PlayStation Vita, written from scratch as homebrew.
It talks to YouTube's InnerTube API straight from the console (no companion server) and plays
videos with the Vita's hardware H.264 and AAC decoders.

VivoTube is not affiliated with, endorsed by, or connected to YouTube or Google.

## Features

- **Home** with topic chips (All, Music, News, Sports, Gaming, Podcasts, Recently uploaded), a
  Shorts shelf, and the latest uploads from your subscriptions
- **Shorts**: a full-height vertical player that loops, with swipe or D-pad navigation, like,
  dislike, comments, share and subscribe
- **Search** with chips (All, Shorts, Videos, Channels, Playlists, Recently uploaded, Live) and a
  Filters dialog (upload date, type, duration, 4K / HD / subtitles / live, sort order), recent
  searches and infinite scrolling
- **Watch page**: like / dislike, Subscribe, Share, Save to playlist, chapters (on the seek bar
  and as a list), loop, double-tap to seek 10 s, captions, quality from 144p to 720p, resume
  where you left off, fullscreen, autoplay with an Up next countdown
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
| Lists | □ | More options (Save, Share, go to channel, subscribe) |
| Home / Search / Channel | L / R | Switch chip or tab |
| Shorts | Up / Down or L / R, swipe | Previous / next Short |
| Shorts | Right | Move to the like / comment / share buttons |
| Watch | × or START | Play / pause |
| Watch | L / R, double-tap the sides | Seek back / forward 10 s |
| Watch | Right / Left | Move between the player and the side panel |
| Watch | □ | Player settings (quality, captions, loop, chapters) |
| Watch | SELECT | Fullscreen |

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
| `google_client.json` (optional) | Your Google OAuth client, for signing in |
| `account.json` | The signed-in account's refresh token. Delete it (or sign out) to forget the account |
| `log.txt` | Diagnostic log, useful for bug reports |
| `clients.json` (optional) | Overrides the built-in InnerTube client definitions (see below) |

## Signing in with Google

Signing in is optional. When you're signed in, VivoTube uses the official YouTube Data API to
read and change your subscriptions, likes and playlists. Google requires every app that uses
that API to have its own OAuth client, and VivoTube doesn't ship one, so you create your own.
Tokens go only between the Vita and Google.

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

Notes:

- While the consent screen's publishing status is *Testing*, Google signs you out after 7 days,
  so you'll need to repeat step 5 now and then.
- The Data API has a daily quota (10,000 units by default), which is plenty for one person.
- Videos still play through InnerTube, signed in or not. Sign-in only changes your library.

If you build VivoTube yourself, you can compile a client in instead: copy
`secrets.cmake.example` to `secrets.cmake` and fill it in. That file is ignored by git.

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
vdpm curl-mbedtls mbedtls zlib zstd bzip2 libvita2d freetype libpng libjpeg-turbo
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

The parsers, fMP4 demuxer, captions, store and update helpers are platform-neutral and tested
on the PC against captured responses in `tests/fixtures/`, using zig's bundled clang:

```bash
python -I tools/probe.py --save
sh tests/host/run.sh
```

The captured responses aren't checked in (they contain other people's content and your IP
address), so capture them first with `probe.py --save`. Tests whose fixture is missing are
skipped.

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
- If YouTube starts requiring PO tokens for the visionOS client, playback will stop until the
  client definitions are updated (see `clients.json` above).

## License

MIT, see [LICENSE](LICENSE). Bundled third-party components:

- [Roboto](https://fonts.google.com/specimen/Roboto) (Apache License 2.0), in
  `assets/fonts/` with its license
- [RapidJSON](https://rapidjson.org) (MIT), in `third_party/rapidjson/`
- Mozilla's CA certificate bundle (MPL 2.0), as `assets/cacert.pem`
- Linked from VitaSDK packages: libcurl, mbedTLS, zlib, zstd, libvita2d, libjpeg-turbo,
  FreeType, libpng, under their own licenses
