// -----------------------------------------------------------------------------
// Gateway → Agent 的同步 HTTP 客户端：阻塞套接字 + poll 连接 + 收满响应。
// 将「TCP/读写到超时、协议损坏」与 Agent 返回的 HTTP 状态码分离，上层据此映射 502/504。
// Week1 刻意同步：先跑通契约与排障语义；后续可换连接池或与 io_uring/协程主线对齐。
// -----------------------------------------------------------------------------
#include "upstream/agent_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
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

bool wait_until_readable(int fd, int timeout_ms, std::string& error) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc > 0 && (pfd.revents & POLLIN)) {
        return true;
    }
    if (rc == 0) {
        error = "read_timeout";
        return false;
    }
    error = std::string("poll_failed: ") + std::strerror(errno);
    return false;
}

bool read_headers_with_timeout(int fd, int timeout_ms, std::string& raw, std::size_t& header_end, std::string& error) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<char> chunk(4096);
    raw.clear();
    header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        const int remain = timeout_ms - elapsed;
        if (remain <= 0) {
            error = "first_chunk_timeout";
            return false;
        }
        if (!wait_until_readable(fd, remain, error)) {
            if (error == "read_timeout") {
                error = "first_chunk_timeout";
            }
            return false;
        }
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            error = "connection_closed_before_headers";
            return false;
        }
        if (n < 0) {
            error = std::string("recv_failed: ") + std::strerror(errno);
            return false;
        }
        raw.append(chunk.data(), static_cast<std::size_t>(n));
        if (raw.size() > 1024 * 1024) {
            error = "response_headers_too_large";
            return false;
        }
        header_end = raw.find("\r\n\r\n");
    }
    return true;
}

int parse_status_line(const std::string& raw_headers, std::string& error) {
    const std::size_t end = raw_headers.find("\r\n");
    if (end == std::string::npos) {
        error = "invalid_http_status_line";
        return 0;
    }
    std::istringstream iss(raw_headers.substr(0, end));
    std::string version;
    int status_code = 0;
    if (!(iss >> version >> status_code)) {
        error = "invalid_http_status_line";
        return 0;
    }
    return status_code;
}

bool read_remaining_with_total_timeout(int fd, std::string& out, int timeout_ms, std::string& error, bool& timed_out) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<char> chunk(4096);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        const int remain = timeout_ms - elapsed;
        if (remain <= 0) {
            timed_out = true;
            error = "stream_total_timeout";
            return false;
        }
        if (!wait_until_readable(fd, remain, error)) {
            if (error == "read_timeout") {
                timed_out = true;
                error = "stream_total_timeout";
            }
            return false;
        }
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            error = std::string("recv_failed: ") + std::strerror(errno);
            return false;
        }
        out.append(chunk.data(), static_cast<std::size_t>(n));
    }
}

bool header_has_chunked_encoding(const std::string& header_block) {
    std::istringstream iss(header_block);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() < 18) {
            continue;
        }
        std::string lower;
        lower.reserve(line.size());
        for (const char c : line) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower.rfind("transfer-encoding:", 0) != 0) {
            continue;
        }
        if (lower.find("chunked") != std::string::npos) {
            return true;
        }
    }
    return false;
}

class ChunkedDecoder {
 public:
    bool feed(std::string_view bytes, const std::function<bool(std::string_view)>& on_chunk, std::string& error) {
        if (finished_) {
            return true;
        }
        buffer_.append(bytes.data(), bytes.size());
        while (true) {
            if (expect_size_) {
                const std::size_t line_end = buffer_.find("\r\n");
                if (line_end == std::string::npos) {
                    return true;
                }
                std::string size_token = buffer_.substr(0, line_end);
                const std::size_t semi = size_token.find(';');
                if (semi != std::string::npos) {
                    size_token = size_token.substr(0, semi);
                }
                unsigned long parsed_size = 0;
                try {
                    parsed_size = std::stoul(size_token, nullptr, 16);
                } catch (...) {
                    error = "invalid_chunk_size";
                    return false;
                }
                current_chunk_size_ = static_cast<std::size_t>(parsed_size);
                buffer_.erase(0, line_end + 2);
                expect_size_ = false;
                if (current_chunk_size_ == 0) {
                    finished_ = true;
                    return true;
                }
            }
            if (buffer_.size() < current_chunk_size_ + 2) {
                return true;
            }
            const std::string_view payload(buffer_.data(), current_chunk_size_);
            if (!on_chunk(payload)) {
                error = "downstream_write_failed";
                return false;
            }
            if (buffer_[current_chunk_size_] != '\r' || buffer_[current_chunk_size_ + 1] != '\n') {
                error = "invalid_chunk_terminator";
                return false;
            }
            buffer_.erase(0, current_chunk_size_ + 2);
            expect_size_ = true;
            current_chunk_size_ = 0;
        }
    }

    bool finished() const { return finished_; }

 private:
    std::string buffer_;
    std::size_t current_chunk_size_{0};
    bool expect_size_{true};
    bool finished_{false};
};

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
    // 未配置超时给默认 8s：避免配置缺省导致 connect/recv 无限挂死占满 accept 线程。
    const int timeout_ms = cfg.agent_timeout_ms > 0 ? cfg.agent_timeout_ms : 8000;
    const int fd = connect_with_timeout(url.host, url.port, timeout_ms, error);
    // 仅 connect_timeout 标 timed_out：与读写超时共用字段名，由上层统一映射为 504 语义。
    if (fd < 0) {
        result.timed_out = (error == "connect_timeout");
        result.error = error.empty() ? "connect_failed" : error;
        return result;
    }

    // 与 connect 同量级套接字超时：防止 TCP 已建立但 Agent 不读/不回导致半包永久阻塞。
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
    // Connection: close 下读到 EOF 即完整响应；recv 超时由上层映射 504，与 connect 超时分流。
    if (!recv_to_end(fd, raw, error, timed_out)) {
        result.timed_out = timed_out;
        result.error = error;
        ::close(fd);
        return result;
    }
    ::close(fd);

    int status_code = 0;
    std::string body;
    // 有字节但无合法 HTTP 头：视为传输/中间层异常，不归因于 Agent 业务逻辑。
    if (!parse_http_response(raw, status_code, body)) {
        result.error = "invalid_http_response";
        return result;
    }

    result.transport_ok = true;
    result.http_status = status_code;
    result.http_body = body;
    return result;
}

AgentStreamResult stream_agent_chat(const GatewayConfigSnapshot& cfg, std::string_view request_json,
                                    const std::function<bool(std::string_view)>& on_chunk) {
    AgentStreamResult result;
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

    const int connect_timeout_ms = cfg.agent_timeout_ms > 0 ? cfg.agent_timeout_ms : 8000;
    const int first_chunk_timeout_ms = cfg.sse_first_chunk_timeout_ms > 0 ? cfg.sse_first_chunk_timeout_ms : 5000;
    const int total_timeout_ms = cfg.sse_total_timeout_ms > 0 ? cfg.sse_total_timeout_ms : 120000;

    std::string error;
    const int fd = connect_with_timeout(url.host, url.port, connect_timeout_ms, error);
    if (fd < 0) {
        result.timed_out = (error == "connect_timeout");
        result.error = error.empty() ? "connect_failed" : error;
        return result;
    }

    std::ostringstream req;
    req << "POST " << endpoint << " HTTP/1.1\r\n"
        << "Host: " << url.host << ":" << url.port << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: application/json\r\n"
        << "Accept: text/event-stream\r\n"
        << "Content-Length: " << request_json.size() << "\r\n\r\n"
        << request_json;
    if (!send_all(fd, req.str(), error)) {
        result.timed_out = (error == "send_timeout");
        result.error = error;
        ::close(fd);
        return result;
    }

    std::string raw_headers;
    std::size_t header_end = std::string::npos;
    const auto stream_started = std::chrono::steady_clock::now();
    if (!read_headers_with_timeout(fd, first_chunk_timeout_ms, raw_headers, header_end, error)) {
        result.timed_out = (error == "first_chunk_timeout");
        result.error = error;
        ::close(fd);
        return result;
    }

    const std::string header_block = raw_headers.substr(0, header_end + 4);
    const bool chunked_encoded = header_has_chunked_encoding(header_block);
    result.http_status = parse_status_line(header_block, error);
    if (result.http_status == 0) {
        result.error = error;
        ::close(fd);
        return result;
    }

    std::string initial_body = raw_headers.substr(header_end + 4);
    if (result.http_status != 200) {
        bool timed_out = false;
        std::string body = std::move(initial_body);
        if (!read_remaining_with_total_timeout(fd, body, total_timeout_ms, error, timed_out)) {
            result.timed_out = timed_out;
            result.error = error;
            ::close(fd);
            return result;
        }
        result.transport_ok = true;
        result.http_body = std::move(body);
        ::close(fd);
        return result;
    }

    bool first_chunk_seen = false;
    ChunkedDecoder chunked_decoder;
    auto forward_data = [&](std::string_view bytes) -> bool {
        if (bytes.empty()) {
            return true;
        }
        if (!first_chunk_seen) {
            first_chunk_seen = true;
            result.ttft_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stream_started)
                    .count());
        }
        return on_chunk(bytes);
    };
    if (!initial_body.empty()) {
        if (chunked_encoded) {
            if (!chunked_decoder.feed(initial_body, forward_data, error)) {
                result.error = error.empty() ? "chunk_decode_failed" : error;
                ::close(fd);
                return result;
            }
        } else if (!forward_data(initial_body)) {
            result.error = "downstream_write_failed";
            ::close(fd);
            return result;
        }
    }

    std::vector<char> chunk(4096);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const int elapsed_ms =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - stream_started).count());
        if (elapsed_ms >= total_timeout_ms) {
            result.timed_out = true;
            result.error = "stream_total_timeout";
            ::close(fd);
            return result;
        }
        int wait_ms = total_timeout_ms - elapsed_ms;
        if (!first_chunk_seen) {
            wait_ms = std::min(wait_ms, std::max(first_chunk_timeout_ms - elapsed_ms, 1));
        }
        if (wait_ms <= 0) {
            result.timed_out = true;
            result.error = first_chunk_seen ? "stream_total_timeout" : "first_chunk_timeout";
            ::close(fd);
            return result;
        }

        if (!wait_until_readable(fd, wait_ms, error)) {
            if (error == "read_timeout") {
                result.timed_out = true;
                result.error = first_chunk_seen ? "stream_total_timeout" : "first_chunk_timeout";
            } else {
                result.error = error;
            }
            ::close(fd);
            return result;
        }

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {
            result.transport_ok = true;
            ::close(fd);
            return result;
        }
        if (n < 0) {
            result.error = std::string("recv_failed: ") + std::strerror(errno);
            ::close(fd);
            return result;
        }

        const std::string_view sv(chunk.data(), static_cast<std::size_t>(n));
        if (chunked_encoded) {
            if (!chunked_decoder.feed(sv, forward_data, error)) {
                result.error = error.empty() ? "chunk_decode_failed" : error;
                ::close(fd);
                return result;
            }
        } else if (!forward_data(sv)) {
            result.error = "downstream_write_failed";
            ::close(fd);
            return result;
        }
    }
}

}  // namespace cyrus::upstream
