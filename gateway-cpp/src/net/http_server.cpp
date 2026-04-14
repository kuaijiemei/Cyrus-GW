#include "net/http_server.h"

#include "api/chat_handler.h"
#include "common/errors.h"
#include "common/json_util.h"
#include "common/logger.h"
#include "limiter/token_bucket.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cyrus::net {
namespace {

void trim_inplace(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
}

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::optional<std::string> header_value(const std::string& headers, std::string_view name_lower) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, colon);
        trim_inplace(key);
        if (to_lower(key) != name_lower) {
            continue;
        }
        auto val = line.substr(colon + 1);
        trim_inplace(val);
        return val;
    }
    return std::nullopt;
}

bool read_more(int fd, std::string& buf, std::size_t min_total) {
    std::vector<char> chunk(8192);
    while (buf.size() < min_total) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(chunk.data(), static_cast<std::size_t>(n));
        if (buf.size() > 1024 * 1024) {
            return false;
        }
    }
    return true;
}

// 读取头部与 Content-Length 指定长度的 body（POST 必需 Content-Length）。
bool read_http_request(int fd, std::string& headers_out, std::string& body_out) {
    std::string buf;
    while (buf.find("\r\n\r\n") == std::string::npos) {
        if (!read_more(fd, buf, buf.size() + 1)) {
            return false;
        }
        if (buf.size() > 65536) {
            return false;
        }
    }
    const auto sep = buf.find("\r\n\r\n");
    headers_out = buf.substr(0, sep);
    body_out = buf.substr(sep + 4);

    std::size_t content_length = 0;
    if (const auto cl = header_value(headers_out, "content-length")) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(cl->c_str(), &end, 10);
        if (end != cl->c_str() && v < 1024ul * 1024ul) {
            content_length = static_cast<std::size_t>(v);
        }
    }

    if (body_out.size() > content_length) {
        body_out.resize(content_length);
    } else if (body_out.size() < content_length) {
        if (!read_more(fd, body_out, content_length)) {
            return false;
        }
        if (body_out.size() > content_length) {
            body_out.resize(content_length);
        }
    }
    return true;
}

bool parse_method_path(const std::string& headers, std::string& method, std::string& path) {
    const auto end = headers.find("\r\n");
    if (end == std::string::npos) {
        return false;
    }
    std::istringstream first(headers.substr(0, end));
    if (!(first >> method >> path)) {
        return false;
    }
    const auto q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    for (auto& c : method) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return true;
}

bool is_health_get(const std::string& method, const std::string& path) {
    return method == "GET" && path == "/health";
}

bool is_chat_post(const std::string& method, const std::string& path) {
    return method == "POST" && path == "/chat";
}

bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool send_all_view(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

void send_json(int fd, int status_code, const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status_code;
    if (status_code == 200) {
        resp << " OK";
    } else if (status_code == 400) {
        resp << " Bad Request";
    } else if (status_code == 404) {
        resp << " Not Found";
    } else if (status_code == 405) {
        resp << " Method Not Allowed";
    } else if (status_code == 429) {
        resp << " Too Many Requests";
    } else {
        resp << " Error";
    }
    resp << "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " << body.size()
       << "\r\n\r\n"
       << body;
    (void)send_all(fd, resp.str());
}

bool send_sse_headers(int fd) {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/event-stream\r\n"
         << "Cache-Control: no-cache\r\n"
         << "Connection: close\r\n\r\n";
    return send_all(fd, resp.str());
}

std::string make_request_id() {
    using clock = std::chrono::steady_clock;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    const std::uint64_t r = (static_cast<std::uint64_t>(gen()) << 32) ^ static_cast<std::uint64_t>(gen()) ^
                            static_cast<std::uint64_t>(ns);
    char buf[40];
    const auto n = std::snprintf(buf, sizeof(buf), "req_%016llx", static_cast<unsigned long long>(r));
    if (n <= 0) {
        return "req_unknown";
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

std::string extract_request_id_from_body(const std::string& body) {
    const std::string key = "\"request_id\"";
    const std::size_t pos = body.find(key);
    if (pos == std::string::npos) {
        return "";
    }
    std::size_t i = pos + key.size();
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
        ++i;
    }
    if (i >= body.size() || body[i] != ':') {
        return "";
    }
    ++i;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
        ++i;
    }
    if (i >= body.size() || body[i] != '"') {
        return "";
    }
    ++i;
    std::string out;
    while (i < body.size()) {
        const char c = body[i++];
        if (c == '"') {
            return out;
        }
        out.push_back(c);
    }
    return "";
}

std::string request_id_from_headers_or_body(const std::string& headers, const std::string& body) {
    if (const auto rid = header_value(headers, "x-request-id"); rid && !rid->empty()) {
        return *rid;
    }
    const std::string rid = extract_request_id_from_body(body);
    if (!rid.empty()) {
        return rid;
    }
    return make_request_id();
}

std::string gateway_error_body(std::string_view error, std::string_view request_id, cyrus::ErrorCode code) {
    std::ostringstream oss;
    oss << "{\"error\":\"" << cyrus::json_escape(error) << "\",\"error_code\":" << static_cast<int>(code)
        << ",\"error_layer\":\"gateway\",\"request_id\":\"" << cyrus::json_escape(request_id) << "\"}";
    return oss.str();
}

std::string rate_limited_body(std::string_view request_id) {
    // 易踩坑点：429 也保持统一 JSON 结构，前端可按 error_code 稳定处理。
    std::ostringstream oss;
    oss << "{\"error\":\"rate_limited\",\"error_code\":" << static_cast<int>(cyrus::ErrorCode::kGwRateLimited)
        << ",\"error_layer\":\"gateway\",\"request_id\":\"" << cyrus::json_escape(request_id) << "\"}";
    return oss.str();
}

}  // namespace

void HttpServer::run() {
    // 模块职责：网关入口统一限流（MVP 先做全局桶），避免超限请求打到上游。
    cyrus::limiter::TokenBucket global_bucket(static_cast<double>(cfg_.rate_limit_capacity),
                                              static_cast<double>(cfg_.rate_limit_refill_per_sec));

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_startup("socket_failed");
        return;
    }
    int yes = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.listen_port);
    if (cfg_.listen_host == "0.0.0.0" || cfg_.listen_host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, cfg_.listen_host.c_str(), &addr.sin_addr) != 1) {
        log_startup("invalid_listen_host");
        ::close(fd);
        return;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind errno=" << errno << "\n";
        log_startup("bind_failed");
        ::close(fd);
        return;
    }
    if (::listen(fd, 128) != 0) {
        log_startup("listen_failed");
        ::close(fd);
        return;
    }

    std::ostringstream oss;
    oss << "listening " << cfg_.listen_host << ":" << cfg_.listen_port;
    log_startup(oss.str());

    for (;;) {
        const int cfd = ::accept(fd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        std::string headers;
        std::string body;
        if (!read_http_request(cfd, headers, body)) {
            ::close(cfd);
            continue;
        }
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();

        std::string method;
        std::string path;
        const std::string request_id = request_id_from_headers_or_body(headers, body);
        if (!parse_method_path(headers, method, path)) {
            send_json(cfd, 400, gateway_error_body("bad_request_line", request_id, cyrus::ErrorCode::kGwBadRequest));
            const auto t1 = clock::now();
            const int ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            log_http_request(request_id, ms, 400, "", "/chat", false, -1, 0, "gateway");
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        if (is_health_get(method, path)) {
            const std::string hb = R"({"status":"ok","service":"gateway"})";
            send_json(cfd, 200, hb);
            const auto t1 = clock::now();
            const int ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            log_http_request("", ms, 200, "", "/health", false);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        if (path == "/chat" && method != "POST") {
            send_json(cfd, 405, R"({"error":"method_not_allowed"})");
            const auto t1 = clock::now();
            const int ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            log_http_request(request_id, ms, 405, "", "/chat", false, -1, 0, "gateway");
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        if (is_chat_post(method, path)) {
            // 关键分支原因：限流必须先于转发，超限请求应立即 429。
            if (!global_bucket.try_consume(1.0)) {
                send_json(cfd, 429, rate_limited_body(request_id));
                const auto t1 = clock::now();
                const int ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                log_http_request(request_id, ms, 429, "rate_limited", "/chat", false, -1, 0, "gateway");
                ::shutdown(cfd, SHUT_RDWR);
                ::close(cfd);
                continue;
            }
            bool sse_headers_sent = false;
            cyrus::api::StreamCallbacks stream_callbacks;
            stream_callbacks.write_chunk = [&](std::string_view chunk) -> bool {
                if (!sse_headers_sent) {
                    if (!send_sse_headers(cfd)) {
                        return false;
                    }
                    sse_headers_sent = true;
                }
                return send_all_view(cfd, chunk);
            };
            const auto r = cyrus::api::handle_post_chat(cfg_, headers, body, &stream_callbacks);
            if ((!r.stream || r.status_code != 200) && !sse_headers_sent) {
                send_json(cfd, r.status_code, r.json_body);
            }
            const auto t1 = clock::now();
            const int ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            log_http_request(r.request_id, ms, r.status_code, r.tool_used, "/chat", r.stream, r.ttft_ms, r.retry_count,
                             r.error_layer);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        send_json(cfd, 404, R"({"error":"not_found"})");
        const auto t1 = clock::now();
        const int ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        log_http_request(request_id, ms, 404, "", path, false, -1, 0, "gateway");
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(fd);
}

}  // namespace cyrus::net
