// YouTube InnerTube API client. All methods block and are meant for worker threads.
//
// Client profiles (name, version, user agent, device fields) come from assets/clients.json and
// can be overridden by ux0:data/VivoTube/clients.json without rebuilding the app.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json_util.h"
#include "models.h"

namespace vt {

struct ClientProfile {
    std::string name;
    int id = 0;
    std::string userAgent;
    std::string version;
    std::string contextJson;  // serialized "context.client" object
    std::vector<std::string> headers;
};

enum class ChannelTab { Videos, Playlists, Home };

// Search filters (the Filters dialog and the chips above results). Values are the ones the
// site's search filter protobuf uses.
struct SearchFilters {
    int sort = 0;      // 0 relevance, 1 rating, 2 upload date, 3 view count
    int upload = 0;    // 0 any, 1 last hour, 2 today, 3 this week, 4 this month, 5 this year
    int type = 0;      // 0 any, 1 video, 2 channel, 3 playlist, 9 Shorts
    int duration = 0;  // 0 any, 4 under 3 minutes, 5 3-20 minutes, 2 over 20 minutes
    bool live = false, hd = false, uhd = false, cc = false;
    bool any() const { return sort || upload || type || duration || live || hd || uhd || cc; }
    bool operator==(const SearchFilters& o) const {
        return sort == o.sort && upload == o.upload && type == o.type && duration == o.duration && live == o.live &&
               hd == o.hd && uhd == o.uhd && cc == o.cc;
    }
};

// The search request's "params" for the filters ("" when none), e.g. videos only -> "EgIQAQ%3D%3D".
std::string search_params(const SearchFilters& f);

class InnerTube {
public:
    // Loads the built-in profile file, then applies the override file if it exists.
    bool loadProfiles(const std::string& builtinPath, const std::string& overridePath, std::string* err);
    void setLocale(const std::string& hl, const std::string& gl);

    // params: the protobuf filter from the search chips / Filters dialog ("" = none).
    Page search(const std::string& query, const std::string& continuation, const std::string& params = "");
    ChannelPage channel(const std::string& channelId, ChannelTab tab, const std::string& continuation);
    PlaylistPage playlist(const std::string& playlistId, const std::string& continuation);
    Page topicFeed(const std::string& channelId);
    // The Home feed ("" continuation = first page). With the account's OAuth token YouTube may
    // personalise it; *personal says whether it did (it ignores or refuses tokens from apps it
    // doesn't recognise, and signed out the feed is empty).
    Page home(const std::string& bearer, const std::string& continuation, bool* personal);
    PlayerInfo player(const std::string& videoId);
    WatchInfo next(const std::string& videoId);
    WatchInfo relatedMore(const std::string& continuation);
    CommentsPage comments(const std::string& continuation);
    // Shorts feed: a seed, then pages of Shorts (sequence = seed.sequence or a page continuation).
    ReelSeed reelSeed();
    Page reelSequence(const std::string& sequence);
    std::vector<Item> channelRss(const std::string& channelId, std::string* err);
    bool captions(const std::string& baseUrl, std::string& out, std::string* err);

    std::string playerUserAgent();
    const std::vector<int>& videoItags() const { return videoItags_; }
    const std::vector<int>& audioItags() const { return audioItags_; }

    // Builds the JSON request body for a role (exposed for tests).
    std::string buildBody(const std::string& role, const std::string& extraJson);

private:
    JDocPtr call(const std::string& role, const char* endpoint, const std::string& extraJson, std::string* err,
                 const std::vector<std::string>& extraHeaders = {});
    bool ensureVisitor(std::string* err);
    const ClientProfile* profileFor(const std::string& role);

    std::mutex mutex_;
    std::mutex visitorFetch_;  // held while the one visitor_id request is in flight
    std::map<std::string, ClientProfile> profiles_;
    std::map<std::string, std::string> roles_;
    std::vector<int> videoItags_{135, 134, 136, 133, 160};
    std::vector<int> audioItags_{140, 139};
    std::string visitor_;
    std::string hl_ = "en", gl_ = "US";
};

}  // namespace vt
