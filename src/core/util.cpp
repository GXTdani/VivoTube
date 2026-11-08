#include "util.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#ifdef __vita__
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#else
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#endif

namespace vt {

static std::mutex g_log_mutex;
static std::string g_log_path;

void log_open(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_path = path;
    // Removed first: Vita3K ignores O_TRUNC, so "w" alone would keep the previous run's log.
    remove(path.c_str());
    if (FILE* f = fopen(path.c_str(), "w")) fclose(f);
}

void log_write(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "[%9.3f] ", monotonic_seconds());
    // Reopened per line so every line reaches the card even if the app crashes (and because
    // Vita3K only writes a host file out when it is closed).
    if (!g_log_path.empty()) {
        if (FILE* f = fopen(g_log_path.c_str(), "a")) {
            fputs(stamp, f);
            fputs(buf, f);
            fputc('\n', f);
            fclose(f);
        }
    }
#ifndef __vita__
    fprintf(stderr, "%s%s\n", stamp, buf);
#endif
}

std::string str_printf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if ((size_t)n < sizeof(buf)) return std::string(buf, (size_t)n);
    std::string out((size_t)n, '\0');
    va_start(ap, fmt);
    vsnprintf(&out[0], out.size() + 1, fmt, ap);
    va_end(ap);
    return out;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

size_t utf8_next(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    i += len;
    while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i > s.size() ? s.size() : i;
}

uint32_t utf8_decode(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return c;
    int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (len == 1 || i + len > s.size()) return 0xFFFD;
    uint32_t cp = c & (0x7F >> len);
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    return cp;
}

bool is_cjk(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) ||  // kana
           (cp >= 0x3400 && cp <= 0x9FFF) ||  // CJK unified
           (cp >= 0xAC00 && cp <= 0xD7AF) ||  // hangul
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

int parse_duration(const std::string& s) {
    int total = 0, part = 0, parts = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            part = part * 10 + (c - '0');
            any = true;
        } else if (c == ':') {
            total = total * 60 + part;
            part = 0;
            parts++;
        } else if (!isspace((unsigned char)c)) {
            return -1;
        }
    }
    if (!any || parts > 2) return -1;
    return total * 60 + part;
}

std::string format_duration(int seconds) {
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600, m = (seconds / 60) % 60, sec = seconds % 60;
    if (h > 0) return str_printf("%d:%02d:%02d", h, m, sec);
    return str_printf("%d:%02d", m, sec);
}

int64_t parse_relative_age(const std::string& in) {
    std::string s;
    for (char c : in) s += (char)tolower((unsigned char)c);
    size_t i = 0;
    while (i < s.size() && !isdigit((unsigned char)s[i])) i++;
    if (i == s.size()) return -1;
    int64_t n = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
    while (i < s.size() && s[i] == ' ') i++;
    std::string unit = s.substr(i);
    struct U {
        const char* name;
        int64_t secs;
    } units[] = {{"second", 1}, {"minute", 60}, {"hour", 3600}, {"day", 86400},
                 {"week", 604800}, {"month", 2592000}, {"year", 31536000}};
    for (const U& u : units)
        if (starts_with(unit, u.name)) return n * u.secs;
    // Compact forms: "2d ago", "3h ago", "1mo ago", "5y ago", "4w ago"
    if (starts_with(unit, "mo")) return n * 2592000;
    if (!unit.empty()) {
        switch (unit[0]) {
            case 's': return n;
            case 'm': return n * 60;
            case 'h': return n * 3600;
            case 'd': return n * 86400;
            case 'w': return n * 604800;
            case 'y': return n * 31536000;
        }
    }
    return -1;
}

std::string format_age(int64_t t, int64_t now) {
    if (t <= 0) return std::string();
    int64_t d = now - t;
    if (d < 0) d = 0;
    struct U {
        const char* name;
        int64_t secs;
    } units[] = {{"year", 31536000}, {"month", 2592000}, {"week", 604800}, {"day", 86400},
                 {"hour", 3600}, {"minute", 60}};
    for (const U& u : units) {
        if (d >= u.secs) {
            int64_t n = d / u.secs;
            return str_printf("%lld %s%s ago", (long long)n, u.name, n == 1 ? "" : "s");
        }
    }
    return "just now";
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int64_t parse_iso8601(const std::string& s) {
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
    int64_t t = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + sec;
    // Timezone suffix: Z, +HH:MM or -HH:MM after the seconds field.
    size_t tpos = s.find('T');
    if (tpos != std::string::npos) {
        size_t z = s.find_first_of("+-Z", tpos);
        if (z != std::string::npos && s[z] != 'Z') {
            int oh = 0, om = 0;
            if (sscanf(s.c_str() + z + 1, "%d:%d", &oh, &om) >= 1) {
                int off = oh * 3600 + om * 60;
                t += s[z] == '+' ? -off : off;
            }
        }
    }
    return t;
}

std::string format_views(int64_t v) {
    if (v < 0) return std::string();
    if (v >= 1000000000) return str_printf("%.1fB views", v / 1e9);
    if (v >= 1000000) return str_printf("%.1fM views", v / 1e6);
    if (v >= 1000) return str_printf("%.1fK views", v / 1e3);
    return str_printf("%lld views", (long long)v);
}

int64_t unix_now() { return (int64_t)time(nullptr); }

double monotonic_seconds() {
#ifdef __vita__
    return (double)sceKernelGetProcessTimeWide() / 1000000.0;
#else
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
#endif
}

bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    remove(tmp.c_str());  // a stale, longer .tmp would otherwise keep its tail where O_TRUNC is ignored
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    // The Vita's rename does not overwrite, so remove first. A crash in between leaves
    // path.tmp behind, which the loader falls back to.
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

void make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' || i + 1 == path.size()) && cur.size() > 1 && cur.back() != ':') {
#ifdef __vita__
            sceIoMkdir(cur.c_str(), 0777);
#elif defined(_WIN32)
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0777);
#endif
        }
    }
}

}  // namespace vt
