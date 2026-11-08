// Converters from raw InnerTube JSON (and the channel RSS feed) into models.
// Every parser is tolerant: unknown shapes are skipped, never fatal.
#pragma once

#include "json_util.h"
#include "models.h"

namespace vt {

// Parses one renderer / view-model (key is its JSON member name). False if not an item.
bool parse_item(const char* key, const JVal& v, Item& out);

// Collects every item and the page continuation under v (Shorts go to page.shorts), skipping
// ads and chrome.
void collect_items(const JVal& v, Page& page);

Page parse_search(const JVal& root);
ChannelPage parse_channel(const JVal& root);
PlaylistPage parse_playlist(const JVal& root);
PlayerInfo parse_player(const JVal& root);
WatchInfo parse_next(const JVal& root);           // first /next response or related continuation
CommentsPage parse_comments(const JVal& root);    // /next with a comments continuation
Page parse_browse_feed(const JVal& root);         // topic channels used on Home
// The Home feed (browse FEwhat_to_watch or its continuation): videos and the continuation.
// *loggedOut is YouTube's own flag for whether the reply was for a signed-out viewer.
Page parse_home_feed(const JVal& root, bool* loggedOut);
ReelSeed parse_reel_seed(const JVal& root);       // reel/reel_item_watch without a video
Page parse_reel_sequence(const JVal& root);       // reel/reel_watch_sequence: Shorts as items

std::vector<Item> parse_rss(const std::string& xml);

// The shorter side of a video stream (its "p" quality, also for vertical video).
int quality_of(const Format& f);

// Picks the best streams for the Vita's decoder. Returns false if none are usable.
bool pick_streams(const PlayerInfo& info, int maxHeight, const std::vector<int>& videoPref,
                  const std::vector<int>& audioPref, Format& video, Format& audio);

}  // namespace vt
