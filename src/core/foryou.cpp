#include "foryou.h"

#include <algorithm>

namespace vt {

namespace {

bool plain_video(const Item& it) { return it.type == ItemType::Video && !it.id.empty() && !it.shorts && !it.live; }

bool same_channel(const Item& a, const Item& b) {
    if (!a.channelId.empty() && !b.channelId.empty()) return a.channelId == b.channelId;
    return !a.channel.empty() && a.channel == b.channel;
}

}  // namespace

std::vector<Item> for_you_seeds(const std::vector<HistoryEntry>& history, const std::vector<Item>& liked, size_t page,
                                size_t perPage, size_t* total) {
    constexpr size_t kMax = 60;
    std::vector<Item> all;
    std::unordered_set<std::string> seen;
    size_t h = 0, l = 0;
    while (all.size() < kMax && (h < history.size() || l < liked.size())) {
        for (; h < history.size(); h++) {
            const Item& it = history[h].item;
            if (plain_video(it) && seen.insert(it.id).second) {
                all.push_back(it);
                h++;
                break;
            }
        }
        if (all.size() >= kMax) break;
        for (; l < liked.size(); l++) {
            const Item& it = liked[l];
            if (plain_video(it) && seen.insert(it.id).second) {
                all.push_back(it);
                l++;
                break;
            }
        }
    }
    if (total) *total = all.size();
    std::vector<Item> out;
    if (all.empty() || perPage == 0) return out;
    size_t n = std::min(perPage, all.size());
    size_t start = (page * perPage) % all.size();
    for (size_t k = 0; k < n; k++) out.push_back(all[(start + k) % all.size()]);
    return out;
}

std::unordered_set<std::string> for_you_exclusions(const std::vector<HistoryEntry>& history,
                                                   const std::vector<std::string>& disliked) {
    std::unordered_set<std::string> out(disliked.begin(), disliked.end());
    for (const HistoryEntry& e : history)
        if (e.length > 0 && e.position >= e.length * 0.8) out.insert(e.item.id);
    return out;
}

std::vector<Item> blend_for_you(ForYouMix& m, size_t limit) {
    std::vector<Item> out;
    std::vector<Item> held;  // related videos waiting for their channel to come round again
    std::vector<size_t> pos(m.related.size(), 0);
    size_t nextList = 0, si = 0, ei = 0;

    auto usable = [&](const Item& it) { return plain_video(it) && !m.exclude.count(it.id); };
    auto crowded = [&](const Item& it) {
        for (size_t k = out.size() >= 2 ? out.size() - 2 : 0; k < out.size(); k++)
            if (same_channel(out[k], it)) return true;
        return false;
    };
    auto take = [&](const Item& it) {
        m.exclude.insert(it.id);
        out.push_back(it);
    };
    // The next usable related video, taking the seeds' lists in turn.
    auto nextRelated = [&](Item& it) {
        for (size_t tries = 0; tries < m.related.size(); tries++) {
            size_t l = nextList;
            nextList = (l + 1) % m.related.size();
            std::vector<Item>& v = m.related[l];
            while (pos[l] < v.size() && !usable(v[pos[l]])) pos[l]++;
            if (pos[l] < v.size()) {
                it = v[pos[l]++];
                return true;
            }
        }
        return false;
    };
    auto fromList = [&](const std::vector<Item>& v, size_t& i) {
        while (i < v.size()) {
            const Item& it = v[i++];
            if (usable(it)) {
                take(it);
                return true;
            }
        }
        return false;
    };

    while (out.size() < limit) {
        if (m.subsEvery > 0 && out.size() % m.subsEvery == m.subsEvery - 1 && fromList(m.subs, si)) continue;
        bool placed = false;
        for (size_t k = 0; k < held.size() && !placed; k++) {
            if (!usable(held[k])) {
                held.erase(held.begin() + (long)k--);
            } else if (!crowded(held[k])) {
                take(held[k]);
                held.erase(held.begin() + (long)k);
                placed = true;
            }
        }
        if (placed) continue;
        Item it;
        while (nextRelated(it)) {
            if (!crowded(it)) {
                take(it);
                placed = true;
                break;
            }
            held.push_back(it);
        }
        if (placed || fromList(m.subs, si) || fromList(m.extra, ei)) continue;
        // Only videos from the channels just shown are left: place them anyway.
        bool any = false;
        for (const Item& h : held) {
            if (out.size() >= limit) break;
            if (usable(h)) {
                take(h);
                any = true;
            }
        }
        held.clear();
        if (!any) break;
    }
    return out;
}

}  // namespace vt
