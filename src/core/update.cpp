#include "update.h"

#include <cctype>
#include <vector>

#include "http.h"
#include "util.h"

namespace vt {

namespace {

std::vector<long> version_parts(const std::string& v) {
    std::vector<long> parts;
    size_t i = 0;
    while (i < v.size() && !isdigit((unsigned char)v[i])) i++;  // "v1.2" -> "1.2"
    while (i < v.size()) {
        if (!isdigit((unsigned char)v[i])) break;  // stop at "-beta" and the like
        long n = 0;
        while (i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
    return parts;
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
    std::vector<long> x = version_parts(a), y = version_parts(b);
    size_t n = x.size() > y.size() ? x.size() : y.size();
    for (size_t i = 0; i < n; i++) {
        long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

// "[text](url)" -> "text"
static std::string strip_links(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '[') {
            size_t close = s.find(']', i + 1);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t paren = s.find(')', close + 2);
                if (paren != std::string::npos) {
                    out += s.substr(i + 1, close - i - 1);
                    i = paren + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

std::string plain_release_notes(const std::string& md) {
    std::string out;
    size_t start = 0;
    while (start <= md.size()) {
        size_t end = md.find('\n', start);
        if (end == std::string::npos) end = md.size();
        std::string line = md.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        while (!t.empty() && t[0] == '#') t.erase(0, 1);  // headings
        t = trim(t);
        if (starts_with(t, "- ") || starts_with(t, "* ")) t = "\xE2\x80\xA2 " + t.substr(2);  // bullets
        t = replace_all(t, "**", "");
        t = replace_all(t, "*", "");
        t = strip_links(t);
        t = replace_all(t, "__", "");
        t = replace_all(t, "`", "");
        if (!(t.empty() && (out.empty() || (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0)))) {
            out += t;
            out += '\n';
        }
        if (end >= md.size()) break;
        start = end + 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

Release parse_release(const JVal& root) {
    Release r;
    if (!root.IsObject()) {
        r.error = "Unexpected reply from GitHub";
        return r;
    }
    std::string msg = jstr(jget(root, "message"));
    r.tag = jstr(jget(root, "tag_name"));
    if (r.tag.empty()) {
        r.error = msg.empty() ? "No releases found" : msg;
        return r;
    }
    r.version = r.tag;
    if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
    r.name = jstr(jget(root, "name"));
    r.notes = plain_release_notes(jstr(jget(root, "body")));
    r.page = jstr(jget(root, "html_url"));
    if (const JVal* assets = jget(root, "assets")) {
        if (assets->IsArray()) {
            for (auto& a : assets->GetArray()) {
                std::string name = jstr(jget(a, "name"));
                if (name.size() < 4 || name.compare(name.size() - 4, 4, ".vpk") != 0) continue;
                r.vpkUrl = jstr(jget(a, "browser_download_url"));
                r.vpkSize = jint(jget(a, "size"), 0);
                break;
            }
        }
    }
    if (r.vpkUrl.empty()) r.error = "The newest release has no .vpk file";
    return r;
}

Release fetch_latest_release(const std::string& repo, const std::string& currentVersion) {
    HttpRequest req;
    req.url = "https://api.github.com/repos/" + repo + "/releases/latest";
    req.userAgent = "VivoTube/" + currentVersion;
    req.headers.push_back("Accept: application/vnd.github+json");
    req.headers.push_back("X-GitHub-Api-Version: 2022-11-28");
    req.connectTimeout = 10;
    req.stallTimeout = 15;
    HttpResponse res = http_request(req);
    Release r;
    if (!res.error.empty()) {
        r.error = res.describe();
        return r;
    }
    std::string err;
    JDocPtr d = jparse(res.body, &err);
    if (!d) {
        r.error = "GitHub: " + (res.ok() ? err : res.describe());
        return r;
    }
    r = parse_release(*d);
    if (!res.ok() && r.error.empty()) r.error = res.describe();
    return r;
}

}  // namespace vt
