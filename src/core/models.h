// Plain data types shared by the parsers, the store and the UI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vt {

enum class ItemType { Video, Playlist, Channel };

struct Item {
    ItemType type = ItemType::Video;
    std::string id;         // videoId / playlistId / channelId
    std::string title;
    std::string channel;    // uploader name
    std::string channelId;
    std::string duration;   // "12:45" (videos)
    std::string views;      // "43K views"
    std::string published;  // "2 weeks ago"
    std::string thumb;      // explicit thumbnail URL; empty means derive from id
    std::string avatar;     // uploader's avatar URL, when the source provides one
    std::string extra;      // "4 videos" for playlists, "1.2M subscribers" for channels
    std::string snippet;    // description excerpt shown in search results
    std::vector<std::string> badges;  // "4K", "CC", "New"
    bool live = false;
    bool verified = false;  // uploader has the verified check
    bool shorts = false;    // a Short (vertical video)
    int64_t time = 0;       // unix publish time when known (RSS); used to sort feeds

    std::string thumbUrl() const;  // thumbnail URL to load for this item
};

struct Page {
    std::vector<Item> items;
    std::vector<Item> shorts;  // Shorts found on the page (shown as a shelf)
    std::string continuation;
    std::string error;
};

struct ChannelPage : Page {
    std::string id, title, description, avatar, subscribers;
};

struct PlaylistPage : Page {
    std::string id, title, author, description, count;
};

struct Format {
    int itag = 0;
    std::string url, mime, codecs;
    int width = 0, height = 0, fps = 0;
    int64_t bitrate = 0, contentLength = 0, approxDurationMs = 0;
    int64_t initStart = -1, initEnd = -1, indexStart = -1, indexEnd = -1;
    int sampleRate = 0, channels = 0;
    bool drc = false;         // dynamic-range-compressed duplicate audio stream
    bool audioDefault = true; // false for dubbed / secondary audio tracks
    bool isVideo() const { return mime.compare(0, 6, "video/") == 0; }
    bool isAudio() const { return mime.compare(0, 6, "audio/") == 0; }
    bool isAvc() const { return codecs.compare(0, 4, "avc1") == 0; }
    bool isAac() const { return codecs.compare(0, 4, "mp4a") == 0; }
    bool hasRanges() const { return indexEnd > 0 && initEnd >= 0; }
};

struct CaptionTrack {
    std::string url, name, lang;
    bool asr = false;  // auto-generated
};

struct PlayerInfo {
    std::string status, reason;
    std::string videoId, title, author, channelId, description;
    int lengthSeconds = 0;
    int64_t viewCount = -1;
    bool live = false;
    std::vector<Format> formats;
    std::vector<CaptionTrack> captions;
    bool ok() const { return status == "OK"; }
};

struct Chapter {
    std::string title;
    int startMs = 0;
};

struct SortOption {
    std::string title, token;  // "Top comments" / "Newest first" and their continuation
};

struct WatchInfo {
    std::vector<Item> related;
    std::string relatedContinuation;
    std::string commentsContinuation;
    std::string channelName, channelId, channelAvatar, subscribers;
    std::string dateText, viewsText, commentCount;
    std::string likes;  // "317K"
    bool verified = false;
    std::vector<Chapter> chapters;
    std::vector<SortOption> commentSorts;
    std::string error;
};

struct Comment {
    std::string author, avatar, text, published, likes, replies;
    std::string repliesToken;  // continuation that loads the replies
    bool pinned = false, creator = false, reply = false;
};

// First step of the Shorts feed: one Short to start with and the sequence that follows it.
struct ReelSeed {
    std::string videoId, sequence, error;
};

struct CommentsPage {
    std::vector<Comment> comments;
    std::string continuation;
    std::string error;
};

}  // namespace vt
