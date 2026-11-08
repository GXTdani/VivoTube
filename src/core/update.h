// Auto-update: asks GitHub for the newest VivoTube release and compares versions.
// Fetching blocks (worker threads); parsing and comparison are pure and host-tested.
#pragma once

#include <cstdint>
#include <string>

#include "json_util.h"

namespace vt {

// The repository releases are published to.
constexpr const char* kUpdateRepo = "hamburger101/VivoTube";

struct Release {
    std::string tag;      // "v1.2.0"
    std::string version;  // "1.2.0"
    std::string name;     // release title
    std::string notes;    // release notes as plain text
    std::string page;     // release web page
    std::string vpkUrl;   // download URL of the .vpk asset
    int64_t vpkSize = 0;
    std::string error;
    bool ok() const { return error.empty() && !version.empty(); }
};

// Parses GitHub's GET /repos/{repo}/releases/latest response.
Release parse_release(const JVal& root);

// Compares dotted versions numerically ("1.10" > "1.9"; a leading "v" is ignored).
// Returns -1, 0 or 1.
int compare_versions(const std::string& a, const std::string& b);

// Turns Markdown release notes into plain lines for the update dialog.
std::string plain_release_notes(const std::string& markdown);

Release fetch_latest_release(const std::string& repo, const std::string& currentVersion);

}  // namespace vt
