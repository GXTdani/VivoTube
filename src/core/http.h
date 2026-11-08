// HTTPS client on libcurl + mbedTLS. Thread-safe: each call borrows a pooled easy handle so
// keep-alive connections are reused, and a shared handle caches DNS and TLS sessions.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace vt {

struct HttpRequest {
    std::string url;
    std::string method = "GET";  // GET or POST
    std::string body;
    std::vector<std::string> headers;  // "Name: value"
    std::string userAgent;
    int64_t rangeStart = -1, rangeEnd = -1;  // inclusive byte range when rangeStart >= 0
    long connectTimeout = 15;
    long stallTimeout = 20;  // abort if no data arrives for this many seconds
    const std::atomic<bool>* cancel = nullptr;
    std::atomic<int64_t>* progressNow = nullptr;    // bytes received so far (optional)
    std::atomic<int64_t>* progressTotal = nullptr;  // expected size once known (optional)
    size_t maxBytes = 64u << 20;
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;  // transport error text; empty if a response was received
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
    std::string describe() const;  // "HTTP 403" / "timeout" etc. for UI messages
};

// caPath: PEM bundle (app0:assets/cacert.pem on the Vita).
bool http_init(const std::string& caPath);
void http_shutdown();
HttpResponse http_request(const HttpRequest& req);
// Logs request/response headers and connection details.
void http_set_debug(bool on);

// Convenience wrappers.
HttpResponse http_get(const std::string& url, const std::string& userAgent = std::string(),
                      const std::atomic<bool>* cancel = nullptr);

}  // namespace vt
