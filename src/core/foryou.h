// "For you" on Home: a feed made from what this Vita and the signed-in account know about the
// viewer, for when YouTube won't personalise its own Home feed for the app. Videos related to
// recent watches and likes come first, with new uploads from subscriptions mixed in.
// Platform-neutral and host-tested.
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "models.h"
#include "store.h"

namespace vt {

// Seeds for one page of the feed: recent watches and liked videos taken in turn (newest first,
// no duplicates, no Shorts or live streams), at most 60 in all. Page p is the perPage seeds
// after the first p * perPage; later pages wrap around. *total gets the size of the full list.
std::vector<Item> for_you_seeds(const std::vector<HistoryEntry>& history, const std::vector<Item>& liked, size_t page,
                                size_t perPage, size_t* total = nullptr);

// Videos the feed should not show again: ones watched to the end (80% or more) and disliked.
std::unordered_set<std::string> for_you_exclusions(const std::vector<HistoryEntry>& history,
                                                   const std::vector<std::string>& disliked);

struct ForYouMix {
    std::vector<std::vector<Item>> related;  // videos related to each seed, in seed order
    std::vector<Item> subs;                  // new uploads from subscriptions, newest first
    std::vector<Item> extra;                 // filler once the others run out (topic feeds)
    std::unordered_set<std::string> exclude; // never shown; what the blend picks is added
    size_t subsEvery = 5;                    // every fifth video is a subscription upload
};

// Takes the related lists in turn (so every seed is represented near the top), drops
// duplicates, Shorts and excluded videos, holds back a video when its channel filled one of the
// two places before it, and puts a subscription upload in every subsEvery-th place.
std::vector<Item> blend_for_you(ForYouMix& mix, size_t limit);

}  // namespace vt
