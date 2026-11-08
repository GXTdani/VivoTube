// Local library kept on the memory card: subscriptions, watch history (with resume points),
// Watch Later, recent searches and settings. Main-thread only.
#pragma once

#include <string>
#include <vector>

#include "models.h"

namespace vt {

struct Subscription {
    std::string id, name, avatar;
};

struct HistoryEntry {
    Item item;
    int64_t watchedAt = 0;
    int position = 0;  // seconds
    int length = 0;    // seconds
};

struct Settings {
    int maxHeight = 480;          // 144..720
    bool captions = false;
    std::string captionLang = "en";
    std::string hl = "en", gl = "US";
    bool autoplay = true;         // continue to next playlist item
    bool rememberPosition = true; // resume from history
    bool checkUpdates = true;     // look for a new VivoTube release at startup
    std::string skipVersion;      // release the user chose to skip
};

// A playlist kept on the memory card ("Save" > New playlist).
struct LocalPlaylist {
    std::string id;  // "VT..." (never a YouTube playlist ID)
    std::string title;
    std::vector<Item> items;  // in playlist order
    int64_t updated = 0;
};

class Store {
public:
    explicit Store(std::string dir = std::string());
    void setDir(const std::string& dir) { dir_ = dir; }
    void load();

    std::vector<Subscription> subs;
    std::vector<HistoryEntry> history;  // newest first
    std::vector<Item> watchLater;       // newest first
    std::vector<std::string> searches;  // newest first
    std::vector<Item> liked;            // Liked videos, newest first
    std::vector<std::string> disliked;  // video IDs
    std::vector<LocalPlaylist> playlists;  // most recently changed first
    Settings settings;

    bool isSubscribed(const std::string& channelId) const;
    void subscribe(const std::string& id, const std::string& name, const std::string& avatar);
    void unsubscribe(const std::string& id);

    void recordWatch(const Item& item, int position, int length);
    int resumePosition(const std::string& videoId) const;  // 0 if none / finished
    float watchedFraction(const std::string& videoId) const;  // 0..1 for the red thumbnail bar, -1 if not watched
    void removeHistory(const std::string& videoId);
    void clearHistory();

    bool inWatchLater(const std::string& videoId) const;
    void toggleWatchLater(const Item& item);  // add or remove
    void removeWatchLater(const std::string& videoId);

    void addSearch(const std::string& q);
    void clearSearches();

    // 1 = liked, -1 = disliked, 0 = neither.
    int rating(const std::string& videoId) const;
    void setRating(const Item& item, int r);

    LocalPlaylist* playlist(const std::string& id);
    std::string createPlaylist(const std::string& title);  // returns the new ID
    void deletePlaylist(const std::string& id);
    bool inPlaylist(const std::string& playlistId, const std::string& videoId) const;
    // Adds the video, or removes it when it is already there. Returns true if it was added.
    bool togglePlaylistItem(const std::string& playlistId, const Item& item);

    void saveSubs();
    void saveHistory();
    void saveWatchLater();
    void saveSearches();
    void saveSettings();
    void saveRatings();
    void savePlaylists();

    static std::string itemToJson(const Item& it);

private:
    std::string path(const char* name) const { return dir_ + "/" + name; }
    std::string dir_;
};

}  // namespace vt
