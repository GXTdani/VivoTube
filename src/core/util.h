// Small portable helpers shared by every layer: logging, strings, time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vt {

// Logging goes to a file on the Vita (ux0:data/VivoTube/log.txt) and stderr on the host.
void log_open(const std::string& path);
void log_write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define VT_LOG(...) ::vt::log_write(__VA_ARGS__)

std::string str_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string url_encode(const std::string& s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const char* prefix);
bool contains(const std::string& s, const char* needle);
std::string replace_all(std::string s, const std::string& from, const std::string& to);

// UTF-8 helpers. Byte offsets always land on code point boundaries.
size_t utf8_next(const std::string& s, size_t i);          // index of the next code point
uint32_t utf8_decode(const std::string& s, size_t i);      // code point starting at i
void utf8_append(std::string& out, uint32_t cp);           // encodes cp (invalid ones as U+FFFD)
bool is_cjk(uint32_t cp);                                  // can break between these characters

// "1:02:03" / "12:45" -> seconds, or -1.
int parse_duration(const std::string& s);
// Seconds -> "1:02:03" or "12:45".
std::string format_duration(int seconds);
// Rough age in seconds for strings like "2 weeks ago" / "Streamed 3 days ago". -1 if unknown.
int64_t parse_relative_age(const std::string& s);
// Unix time -> "3 days ago".
std::string format_age(int64_t unixTime, int64_t now);
// "2026-10-23T16:00:36+00:00" -> unix time (UTC), 0 on failure.
int64_t parse_iso8601(const std::string& s);
// 448014458 -> "448M views".
std::string format_views(int64_t views);

int64_t unix_now();
double monotonic_seconds();

bool read_file(const std::string& path, std::string& out);
// Writes to path.tmp, then replaces path. Returns false on any I/O error.
bool write_file_atomic(const std::string& path, const std::string& data);
void make_dirs(const std::string& path);

}  // namespace vt
