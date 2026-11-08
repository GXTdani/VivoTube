// YouTube timedtext "json3" captions.
#pragma once

#include <string>
#include <vector>

namespace vt {

struct Cue {
    int startMs = 0, endMs = 0;
    std::string text;
};

class Captions {
public:
    bool parse(const std::string& json3, std::string* err = nullptr);
    // Text to show at time t (lines joined with '\n'); empty when nothing is on screen.
    std::string at(double seconds) const;
    size_t size() const { return cues_.size(); }
    const std::vector<Cue>& cues() const { return cues_; }

private:
    std::vector<Cue> cues_;  // sorted by start
};

}  // namespace vt
