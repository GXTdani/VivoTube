// "Sign in with Google" through the OAuth 2.0 device flow (the one TVs use: the Vita shows a
// code, you approve it on a phone or computer), plus the YouTube Data API v3 calls VivoTube
// syncs with: subscriptions, ratings, playlists and liked videos.
//
// The OAuth client is the user's own ("TVs and Limited Input devices" type) from Google Cloud,
// read from ux0:data/VivoTube/google_client.json or compiled in from secrets.cmake.
// Network methods block; call them on worker threads. State is guarded by a mutex.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "json_util.h"
#include "models.h"
#include "store.h"

namespace vt {

struct DeviceCode {
    std::string deviceCode, userCode, verifyUrl;
    int interval = 5, expiresIn = 1800;
    std::string error;
};

struct TokenResult {
    std::string access, refresh;
    int expiresIn = 0;
    std::string error;  // "authorization_pending", "slow_down", "access_denied", "expired_token", ...
    bool pending() const { return error == "authorization_pending" || error == "slow_down"; }
};

struct GoogleProfile {
    std::string channelId, name, handle, avatar;
};

// Pure parsers (host-tested).
DeviceCode parse_device_code(const JVal& root);
TokenResult parse_token(const JVal& root);
bool parse_profile(const JVal& root, GoogleProfile& out);
// subscriptions.list
void parse_api_subscriptions(const JVal& root, std::vector<Subscription>& out, std::string& nextPage);
// playlists.list
void parse_api_playlists(const JVal& root, std::vector<Item>& out, std::string& nextPage);
// videos.list or playlistItems.list
void parse_api_videos(const JVal& root, std::vector<Item>& out, std::string& nextPage);
std::string parse_api_error(const JVal& root);  // "" if the reply is not an error
int iso8601_seconds(const std::string& d);      // "PT1H2M3S" -> 3723

class GoogleAccount {
public:
    // Loads the OAuth client and any saved sign-in from dir.
    void init(const std::string& dir);
    bool configured() const;
    bool signedIn() const;
    GoogleProfile profile() const;
    std::string clientFile() const { return dir_ + "/google_client.json"; }

    DeviceCode requestCode();
    TokenResult pollToken(const std::string& deviceCode);
    // Stores the tokens and loads the account's channel name and picture.
    bool completeSignIn(const TokenResult& t, std::string* err);
    void signOut();  // revokes the token with Google and forgets it

    bool mySubscriptions(std::vector<Subscription>& out, std::string* err);
    bool subscribe(const std::string& channelId, std::string* err);
    bool unsubscribe(const std::string& channelId, std::string* err);
    bool rate(const std::string& videoId, int rating, std::string* err);  // 1, -1, 0
    bool getRating(const std::string& videoId, int& rating, std::string* err);
    bool myPlaylists(std::vector<Item>& out, std::string* err);
    Page playlistItems(const std::string& playlistId, const std::string& pageToken);
    Page likedVideos(const std::string& pageToken);
    bool addToPlaylist(const std::string& playlistId, const std::string& videoId, std::string* err);
    // A current access token (refreshed when it has expired), for other Google APIs.
    bool accessToken(std::string& token, std::string* err);

private:
    // Calls https://www.googleapis.com/youtube/v3/<path>. Returns the parsed body (null on error).
    JDocPtr api(const std::string& method, const std::string& path, const std::string& body, std::string* err,
                long* status = nullptr);
    void save();

    mutable std::mutex m_;
    std::string dir_;
    std::string clientId_, clientSecret_;
    std::string refresh_, access_;
    int64_t accessExpiry_ = 0;
    GoogleProfile profile_;
};

}  // namespace vt
