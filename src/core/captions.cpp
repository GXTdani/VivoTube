#include "captions.h"

#include <algorithm>

#include "json_util.h"
#include "util.h"

namespace vt {

bool Captions::parse(const std::string& text, std::string* err) {
    cues_.clear();
    JDocPtr doc = jparse(text, err);
    if (!doc) return false;
    const JVal* events = jget(*doc, "events");
    if (!events || !events->IsArray()) {
        if (err) *err = "captions have no events";
        return false;
    }
    for (auto& e : events->GetArray()) {
        if (jint(jget(e, "aAppend"), 0)) continue;  // ASR line-continuation markers
        const JVal* segs = jget(e, "segs");
        if (!segs || !segs->IsArray()) continue;
        std::string t;
        for (auto& s : segs->GetArray()) t += jstr(jget(s, "utf8"));
        t = trim(t);
        if (t.empty()) continue;
        Cue c;
        c.startMs = (int)jint(jget(e, "tStartMs"), 0);
        c.endMs = c.startMs + (int)jint(jget(e, "dDurationMs"), 2000);
        c.text = t;
        cues_.push_back(std::move(c));
    }
    std::stable_sort(cues_.begin(), cues_.end(), [](const Cue& a, const Cue& b) { return a.startMs < b.startMs; });
    // Auto-generated tracks overlap heavily; end each cue when the next one starts so at most
    // one rolling line pair is on screen.
    for (size_t i = 0; i + 1 < cues_.size(); i++) {
        if (cues_[i].endMs > cues_[i + 1].startMs && cues_[i + 1].startMs > cues_[i].startMs)
            cues_[i].endMs = cues_[i + 1].startMs;
    }
    return true;
}

std::string Captions::at(double seconds) const {
    int ms = (int)(seconds * 1000.0);
    auto it = std::upper_bound(cues_.begin(), cues_.end(), ms, [](int v, const Cue& c) { return v < c.startMs; });
    std::string out;
    // Walk back over cues that started before now and are still showing (usually 0 or 1).
    int checked = 0;
    while (it != cues_.begin() && checked < 4) {
        --it;
        checked++;
        if (it->endMs > ms) out = out.empty() ? it->text : it->text + "\n" + out;
    }
    return out;
}

}  // namespace vt
