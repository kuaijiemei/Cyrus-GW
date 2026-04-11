#include "net/http_server.h"

#include "api/chat_handler.h"
#include "common/logger.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
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

void send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            break;
        }
        off += static_cast<std::size_t>(n);
    }
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
    } else {
        resp << " Error";
    }
    resp << "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " << body.size()
       << "\r\n\r\n"
       << body;
    send_all(fd, resp.str());
}

}  // namespace

void HttpServer::run() {
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

        std::string method;
        std::string path;
        if (!parse_method_path(headers, method, path)) {
            send_json(cfd, 400, R"({"error":"bad_request_line"})");
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();

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
            log_http_request("", ms, 405, "", "/chat", false);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        if (is_chat_post(method, path)) {
            const auto r = cyrus::api::handle_post_chat(cfg_, headers, body);
            send_json(cfd, r.status_code, r.json_body);
            const auto t1 = clock::now();
            const int ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            log_http_request(r.request_id, ms, r.status_code, r.tool_used, "/chat", r.stream);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            continue;
        }

        send_json(cfd, 404, R"({"error":"not_found"})");
        const auto t1 = clock::now();
        const int ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        log_http_request("", ms, 404, "", path, false);
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(fd);
}

}  // namespace cyrus::net
