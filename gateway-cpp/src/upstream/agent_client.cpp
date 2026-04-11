#include "upstream/agent_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cyrus::upstream {
namespace {

struct ParsedUrl {
    std::string host;
    std::string port{"80"};
    std::string base_path;
};

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool parse_http_url(const std::string& raw, ParsedUrl& out) {
    std::string url = raw;
    if (url.empty()) {
        return false;
    }
    if (!starts_with(url, "http://")) {
        return false;
    }
    url = url.substr(7);

    const std::size_t slash = url.find('/');
    std::string host_port = slash == std::string::npos ? url : url.substr(0, slash);
    out.base_path = slash == std::string::npos ? "" : url.substr(slash);

    if (host_port.empty()) {
        return false;
    }

    const std::size_t colon = host_port.rfind(':');
    if (colon == std::string::npos) {
        out.host = host_port;
        out.port = "80";
    } else {
        out.host = host_port.substr(0, colon);
        out.port = host_port.substr(colon + 1);
        if (out.port.empty()) {
            out.port = "80";
        }
    }

    if (out.host.empty()) {
        return false;
    }
    return true;
}

int connect_with_timeout(const std::string& host, const std::string& port, int timeout_ms, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0) {
        error = std::string("getaddrinfo_failed: ") + ::gai_strerror(gai);
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            if (flags >= 0) {
                (void)::fcntl(fd, F_SETFL, flags);
            }
            ::freeaddrinfo(res);
            return fd;
        }

        if (errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int poll_rc = ::poll(&pfd, 1, timeout_ms);
            if (poll_rc > 0 && (pfd.revents & POLLOUT)) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
                    if (flags >= 0) {
                        (void)::fcntl(fd, F_SETFL, flags);
                    }
                    ::freeaddrinfo(res);
                    return fd;
                }
                if (so_error != 0) {
                    error = std::string("connect_failed: ") + std::strerror(so_error);
                }
            } else if (poll_rc == 0) {
                error = "connect_timeout";
            } else {
                error = std::string("poll_failed: ") + std::strerror(errno);
            }
        } else {
            error = std::string("connect_failed: ") + std::strerror(errno);
        }

        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(res);
    return -1;
}

bool send_all(int fd, const std::string& data, std::string& error) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                error = "send_timeout";
            } else {
                error = std::string("send_failed: ") + std::strerror(errno);
            }
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool recv_to_end(int fd, std::string& out, std::string& error, bool& timed_out) {
    out.clear();
    std::vector<char> chunk(8192);
    while (true) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                timed_out = true;
                error = "recv_timeout";
            } else {
                error = std::string("recv_failed: ") + std::strerror(errno);
            }
            return false;
        }
        out.append(chunk.data(), static_cast<std::size_t>(n));
        if (out.size() > 4 * 1024 * 1024) {
            error = "response_too_large";
            return false;
        }
    }
}

bool parse_http_response(const std::string& raw, int& status_code, std::string& body) {
    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }
    const std::string status_line = raw.substr(0, raw.find("\r\n"));
    std::istringstream iss(status_line);
    std::string version;
    if (!(iss >> version >> status_code)) {
        return false;
    }
    body = raw.substr(header_end + 4);
    return true;
}

}  // namespace

AgentClientResult post_agent_chat(const GatewayConfigSnapshot& cfg, std::string_view request_json) {
    AgentClientResult result;
    ParsedUrl url;
    if (!parse_http_url(cfg.agent_base_url, url)) {
        result.error = "invalid_agent_base_url";
        return result;
    }

    std::string endpoint = "/agent/chat";
    if (!url.base_path.empty()) {
        endpoint = url.base_path;
        if (!endpoint.empty() && endpoint.back() == '/') {
            endpoint.pop_back();
        }
        endpoint += "/agent/chat";
    }

    std::string error;
    const int timeout_ms = cfg.agent_timeout_ms > 0 ? cfg.agent_timeout_ms : 8000;
    const int fd = connect_with_timeout(url.host, url.port, timeout_ms, error);
    if (fd < 0) {
        result.timed_out = (error == "connect_timeout");
        result.error = error.empty() ? "connect_failed" : error;
        return result;
    }

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::ostringstream req;
    req << "POST " << endpoint << " HTTP/1.1\r\n"
        << "Host: " << url.host << ":" << url.port << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << request_json.size() << "\r\n\r\n"
        << request_json;

    if (!send_all(fd, req.str(), error)) {
        result.timed_out = (error == "send_timeout");
        result.error = error;
        ::close(fd);
        return result;
    }

    std::string raw;
    bool timed_out = false;
    if (!recv_to_end(fd, raw, error, timed_out)) {
        result.timed_out = timed_out;
        result.error = error;
        ::close(fd);
        return result;
    }
    ::close(fd);

    int status_code = 0;
    std::string body;
    if (!parse_http_response(raw, status_code, body)) {
        result.error = "invalid_http_response";
        return result;
    }

    result.transport_ok = true;
    result.http_status = status_code;
    result.http_body = body;
    return result;
}

}  // namespace cyrus::upstream
