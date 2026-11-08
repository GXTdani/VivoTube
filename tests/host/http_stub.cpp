// Host tests never touch the network; the InnerTube request builder links against this stub.
#include "../../src/core/http.h"

namespace vt {

std::string HttpResponse::describe() const { return error.empty() ? "HTTP " + std::to_string(status) : error; }
bool http_init(const std::string&) { return true; }
void http_shutdown() {}

HttpResponse http_request(const HttpRequest&) {
    HttpResponse r;
    r.error = "network disabled in host tests";
    return r;
}

HttpResponse http_get(const std::string& url, const std::string&, const std::atomic<bool>*) {
    HttpRequest r;
    r.url = url;
    return http_request(r);
}

}  // namespace vt
