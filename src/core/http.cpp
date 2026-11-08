#include "http.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <zlib.h>

#include <atomic>
#include <cctype>
#include <map>
#include <mutex>

#include "util.h"

namespace vt {

namespace {

std::string g_ca, g_caPem;  // bundle path, and its contents read once
CURLSH* g_share = nullptr;
std::mutex g_share_locks[CURL_LOCK_DATA_LAST];
std::mutex g_pool_mutex;
std::vector<CURL*> g_pool;
const char* kDefaultUA = "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko) Silk/3.2";

std::atomic<bool> g_debug{false};
std::atomic<bool> g_no_compression{false};

// Our own DNS cache in front of curl: one resolver call at a time (the system resolver is not
// safe to call from several threads at once in every environment), cached for 10 minutes.
std::mutex g_dns_mutex;
std::map<std::string, std::pair<std::string, double>> g_dns;

std::string resolve_host(const std::string& host) {
    std::lock_guard<std::mutex> lock(g_dns_mutex);
    double now = monotonic_seconds();
    auto it = g_dns.find(host);
    if (it != g_dns.end() && it->second.second > now) return it->second.first;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string ip;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc == 0 && res) {
        for (addrinfo* a = res; a; a = a->ai_next) {
            if (a->ai_family != AF_INET) continue;
            char buf[INET_ADDRSTRLEN] = {};
            const sockaddr_in* sin = (const sockaddr_in*)a->ai_addr;
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                ip = buf;
                break;
            }
        }
        freeaddrinfo(res);
    }
    if (g_debug) VT_LOG("dns: %s -> %s (rc %d)", host.c_str(), ip.empty() ? "?" : ip.c_str(), rc);
    if (!ip.empty()) g_dns[host] = {ip, now + 600};
    return ip;
}

// "https://host:port/path" -> host and port.
bool split_host(const std::string& url, std::string& host, int& port) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    port = url.compare(0, p, "https") == 0 ? 443 : 80;
    size_t start = p + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string hp = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos && hp.find(']') == std::string::npos) {
        port = atoi(hp.c_str() + colon + 1);
        hp = hp.substr(0, colon);
    }
    host = hp;
    return !host.empty();
}

int debug_cb(CURL*, curl_infotype type, char* data, size_t size, void*) {
    if (type != CURLINFO_TEXT && type != CURLINFO_HEADER_IN && type != CURLINFO_HEADER_OUT) return 0;
    std::string s(data, size > 600 ? 600 : size);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    for (char& ch : s)
        if (ch == '\r' || ch == '\n') ch = ' ';
    const char* tag = type == CURLINFO_TEXT ? "*" : type == CURLINFO_HEADER_IN ? "<" : ">";
    VT_LOG("curl %s %s", tag, s.c_str());
    return 0;
}

void lock_cb(CURL*, curl_lock_data data, curl_lock_access, void*) { g_share_locks[data].lock(); }
void unlock_cb(CURL*, curl_lock_data data, void*) { g_share_locks[data].unlock(); }

struct Sink {
    std::string* out;
    size_t max;
    bool overflow = false;
    std::string encoding;  // Content-Encoding of the final response, lower case
};

// Response headers: remember Content-Encoding. A new status line (redirect, 100-continue)
// starts over.
size_t header_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    Sink* s = (Sink*)user;
    size_t n = size * nmemb;
    std::string line(ptr, n);
    if (line.compare(0, 5, "HTTP/") == 0) {
        s->encoding.clear();
    } else if (line.size() > 17) {
        std::string key = line.substr(0, 17);
        for (char& c : key) c = (char)tolower((unsigned char)c);
        if (key == "content-encoding:") {
            std::string v = trim(line.substr(17));
            for (char& c : v) c = (char)tolower((unsigned char)c);
            s->encoding = v;
        }
    }
    return n;
}

// gzip (RFC 1952) without zlib's gzip support, which the VitaSDK zlib is built without: skip the
// member header by hand, then raw-inflate the deflate stream.
bool gunzip(const std::string& in, std::string& out, size_t maxOut, std::string* err) {
    const unsigned char* p = (const unsigned char*)in.data();
    size_t n = in.size(), i = 10;
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) {
        if (err) *err = "not gzip data";
        return false;
    }
    unsigned flags = p[3];
    if (flags & 4) {  // FEXTRA
        if (i + 2 > n) return false;
        i += 2 + (p[i] | (p[i + 1] << 8));
    }
    if (flags & 8)  // FNAME
        while (i < n && p[i++] != 0) {}
    if (flags & 16)  // FCOMMENT
        while (i < n && p[i++] != 0) {}
    if (flags & 2) i += 2;  // FHCRC
    if (i >= n) {
        if (err) *err = "truncated gzip header";
        return false;
    }
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
        if (err) *err = "inflateInit failed";
        return false;
    }
    out.clear();
    z.next_in = (Bytef*)(p + i);
    z.avail_in = (uInt)(n - i);
    char buf[32768];
    int rc = Z_OK;
    while (rc == Z_OK) {
        z.next_out = (Bytef*)buf;
        z.avail_out = sizeof(buf);
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        out.append(buf, sizeof(buf) - z.avail_out);
        if (out.size() > maxOut) {
            rc = Z_MEM_ERROR;
            break;
        }
        if (rc == Z_OK && z.avail_in == 0) break;  // truncated input
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END) {
        if (err) *err = out.size() > maxOut ? "response too large" : str_printf("gzip data error (%d)", rc);
        return false;
    }
    return true;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* user) {
    Sink* s = (Sink*)user;
    size_t n = size * nmemb;
    if (s->out->size() + n > s->max) {
        s->overflow = true;
        return 0;
    }
    s->out->append(ptr, n);
    return n;
}

int progress_cb(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    const HttpRequest* req = (const HttpRequest*)user;
    if (req->progressNow) req->progressNow->store((int64_t)dlnow);
    if (req->progressTotal && dltotal > 0) req->progressTotal->store((int64_t)dltotal);
    return (req->cancel && req->cancel->load()) ? 1 : 0;
}

CURL* acquire() {
    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        if (!g_pool.empty()) {
            CURL* c = g_pool.back();
            g_pool.pop_back();
            return c;
        }
    }
    return curl_easy_init();
}

void release(CURL* c) {
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    if (g_pool.size() < 8) g_pool.push_back(c);
    else curl_easy_cleanup(c);
}

}  // namespace

std::string HttpResponse::describe() const {
    if (!error.empty()) return error;
    return str_printf("HTTP %ld", status);
}

bool http_init(const std::string& caPath) {
    g_ca = caPath;
    // Handed to every request from memory: many requests reading the file at once sometimes
    // failed with "Problem with the SSL CA cert".
    if (!caPath.empty() && !read_file(caPath, g_caPem)) g_caPem.clear();
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) return false;
    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, lock_cb);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, unlock_cb);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
    curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    VT_LOG("http: curl %s, %s", v->version, v->ssl_version ? v->ssl_version : "no TLS");
    return true;
}

void http_shutdown() {
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    for (CURL* c : g_pool) curl_easy_cleanup(c);
    g_pool.clear();
    if (g_share) curl_share_cleanup(g_share);
    g_share = nullptr;
    curl_global_cleanup();
}

HttpResponse http_request(const HttpRequest& req) {
    HttpResponse res;
    CURL* c = acquire();
    if (!c) {
        res.error = "out of memory";
        return res;
    }
    curl_easy_reset(c);
    Sink sink{&res.body, req.maxBytes};
    curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    // gzip for API / JSON responses (a browse page is ~900 KB raw, ~10x smaller gzipped), decoded
    // here rather than by curl (see gunzip). Not for byte ranges of media, which is incompressible.
    // Dropped for good if decoding ever fails.
    const bool compressed = !g_no_compression && req.rangeStart < 0;
    curl_easy_setopt(c, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    if (g_debug) {
        curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, debug_cb);
    }
    curl_slist* resolve = nullptr;
    {
        std::string host;
        int port = 0;
        if (split_host(req.url, host, port)) {
            std::string ip = resolve_host(host);
            if (!ip.empty()) {
                resolve = curl_slist_append(nullptr, str_printf("%s:%d:%s", host.c_str(), port, ip.c_str()).c_str());
                curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
            }
        }
    }
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    bool caSet = false;
    if (!g_caPem.empty()) {
        curl_blob blob{(void*)g_caPem.data(), g_caPem.size(), CURL_BLOB_NOCOPY};
        caSet = curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob) == CURLE_OK;
    }
    if (!caSet && !g_ca.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, g_ca.c_str());
    if (g_share) curl_easy_setopt(c, CURLOPT_SHARE, g_share);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, req.connectTimeout);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, req.stallTimeout);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, req.userAgent.empty() ? kDefaultUA : req.userAgent.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
    if (req.cancel || req.progressNow || req.progressTotal) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void*)&req);
    } else {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    }
    std::string range;
    if (req.rangeStart >= 0) {
        range = req.rangeEnd >= 0 ? str_printf("%lld-%lld", (long long)req.rangeStart, (long long)req.rangeEnd)
                                  : str_printf("%lld-", (long long)req.rangeStart);
        curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    }
    if (req.method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {  // DELETE / PUT for the Data API
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }
    curl_slist* hdrs = nullptr;
    for (const std::string& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    curl_slist* hdrsPlain = hdrs;
    if (compressed) {
        // Appended last so the plain list (a prefix of it) can be reused for a retry.
        curl_slist* gz = curl_slist_append(nullptr, "Accept-Encoding: gzip");
        if (hdrs) {
            curl_slist* t = hdrs;
            while (t->next) t = t->next;
            t->next = gz;
        } else {
            hdrs = gz;
        }
    }
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK && sink.encoding == "gzip") {
        std::string plain, gerr;
        size_t packed = res.body.size();
        if (gunzip(res.body, plain, req.maxBytes, &gerr)) {
            if (g_debug) VT_LOG("http: gzip %u -> %u bytes", (unsigned)packed, (unsigned)plain.size());
            res.body.swap(plain);
        } else if (gerr == "response too large") {
            rc = CURLE_WRITE_ERROR;
            sink.overflow = true;
        } else {
            if (!g_no_compression.exchange(true))
                VT_LOG("http: gzip response could not be decoded (%s); compression is now off", gerr.c_str());
            res.body.clear();
            sink = Sink{&res.body, req.maxBytes};
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrsPlain);
            rc = curl_easy_perform(c);
        }
    } else if (rc == CURLE_OK && !sink.encoding.empty() && sink.encoding != "identity") {
        res.error = "unsupported response encoding: " + sink.encoding;
    }
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK) res.error = "cancelled";
        else if (sink.overflow) res.error = "response too large";
        else res.error = curl_easy_strerror(rc);
    }
    if (hdrs) curl_slist_free_all(hdrs);  // includes the gzip entry when it was appended
    if (resolve) curl_slist_free_all(resolve);
    release(c);
    return res;
}

void http_set_debug(bool on) { g_debug = on; }

HttpResponse http_get(const std::string& url, const std::string& userAgent, const std::atomic<bool>* cancel) {
    HttpRequest r;
    r.url = url;
    r.userAgent = userAgent;
    r.cancel = cancel;
    return http_request(r);
}

}  // namespace vt
