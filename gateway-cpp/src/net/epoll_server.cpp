// ---------------------------------------------------------------------------
// epoll + 线程池 HTTP 服务（对照实现）。
// 架构：主线程 epoll_wait 监听 listen fd，accept 后将 client fd 入队 TaskQueue；
//       WorkerPool 中 N 个 worker 线程从队列取 fd，以阻塞 I/O 完成
//       "读请求 → 处理 → 写响应 → 关闭"。
//
// 关键分支原因：
//   - 仅对 listen fd 做 epoll，不对 client fd 注册 epoll 事件——worker 直接阻塞
//     recv/send，简化实现且与 io_uring 主实现形成 Reactor vs Proactor 对比。
//   - 调度逻辑已抽象到 scheduler/TaskQueue + WorkerPool，支持
//     queue_wait_ms 统计、队列容量限制、超时任务自动取消。
// ---------------------------------------------------------------------------
#include "net/epoll_server.h"

#include "api/chat_handler.h"
#include "common/errors.h"
#include "common/logger.h"
#include "limiter/token_bucket.h"
#include "net/http_common.h"
#include "scheduler/task_queue.h"
#include "scheduler/worker_pool.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cyrus::net {
namespace {

int create_listener(const GatewayConfigSnapshot& cfg) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg.listen_port);
    if (cfg.listen_host == "0.0.0.0" || cfg.listen_host.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, cfg.listen_host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 512) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ---- blocking I/O helpers ----

bool read_more(int fd, std::string& buf, std::size_t min_total) {
    std::vector<char> chunk(8192);
    while (buf.size() < min_total) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n <= 0) return false;
        buf.append(chunk.data(), static_cast<std::size_t>(n));
        if (buf.size() > 1024 * 1024) return false;
    }
    return true;
}

bool read_http_request(int fd, std::string& headers_out, std::string& body_out) {
    std::string buf;
    while (buf.find("\r\n\r\n") == std::string::npos) {
        if (!read_more(fd, buf, buf.size() + 1)) return false;
        if (buf.size() > 65536) return false;
    }
    const auto sep = buf.find("\r\n\r\n");
    headers_out = buf.substr(0, sep);
    body_out    = buf.substr(sep + 4);

    const std::size_t content_length = http::parse_content_length(headers_out);
    if (body_out.size() > content_length) {
        body_out.resize(content_length);
    } else if (body_out.size() < content_length) {
        if (!read_more(fd, body_out, content_length)) return false;
        if (body_out.size() > content_length) body_out.resize(content_length);
    }
    return true;
}

bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool send_all_view(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// ---- worker handler：完整处理一条连接 ----

void handle_connection(int client_fd, int queue_wait_ms,
                       const GatewayConfigSnapshot& cfg,
                       cyrus::limiter::TokenBucket& bucket) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    std::string headers, body;
    if (!read_http_request(client_fd, headers, body)) {
        ::close(client_fd);
        return;
    }

    std::string method, path;
    const std::string request_id = http::request_id_from_headers_or_body(headers, body);
    if (!http::parse_method_path(headers, method, path)) {
        auto resp = http::format_json_response(
            400, http::gateway_error_body("bad_request_line", request_id,
                                          cyrus::ErrorCode::kGwBadRequest));
        (void)send_all(client_fd, resp);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request(request_id, ms, 400, "", "/chat", false, -1, 0, "gateway", queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    if (http::is_health_get(method, path)) {
        auto resp = http::format_json_response(200, http::format_health_body());
        (void)send_all(client_fd, resp);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request("", ms, 200, "", "/health", false, -1, 0, "", queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    if (path == "/chat" && method != "POST") {
        auto resp = http::format_json_response(405, R"({"error":"method_not_allowed"})");
        (void)send_all(client_fd, resp);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request(request_id, ms, 405, "", "/chat", false, -1, 0, "gateway", queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    if (http::is_chat_post(method, path)) {
        if (!bucket.try_consume(1.0)) {
            auto resp = http::format_json_response(429, http::rate_limited_body(request_id));
            (void)send_all(client_fd, resp);
            const int ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
            log_http_request(request_id, ms, 429, "rate_limited", "/chat", false, -1, 0, "gateway", queue_wait_ms);
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            return;
        }

        bool sse_headers_sent = false;
        cyrus::api::StreamCallbacks stream_callbacks;
        stream_callbacks.write_chunk = [&](std::string_view chunk) -> bool {
            if (!sse_headers_sent) {
                if (!send_all(client_fd, http::format_sse_headers())) return false;
                sse_headers_sent = true;
            }
            return send_all_view(client_fd, chunk);
        };

        const auto r = cyrus::api::handle_post_chat(cfg, headers, body, &stream_callbacks);
        if ((!r.stream || r.status_code != 200) && !sse_headers_sent) {
            auto resp = http::format_json_response(r.status_code, r.json_body);
            (void)send_all(client_fd, resp);
        }
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request(r.request_id, ms, r.status_code, r.tool_used, "/chat",
                         r.stream, r.ttft_ms, r.retry_count, r.error_layer, queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        return;
    }

    // 404
    auto resp = http::format_json_response(404, R"({"error":"not_found"})");
    (void)send_all(client_fd, resp);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
    log_http_request(request_id, ms, 404, "", path, false, -1, 0, "gateway", queue_wait_ms);
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

// ---- public entry ----

void EpollServer::run() {
    const int listen_fd = create_listener(cfg_);
    if (listen_fd < 0) {
        log_startup("epoll_listen_failed");
        return;
    }
    set_nonblocking(listen_fd);

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        log_startup("epoll_create_failed");
        ::close(listen_fd);
        return;
    }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) != 0) {
        log_startup("epoll_ctl_failed");
        ::close(epoll_fd);
        ::close(listen_fd);
        return;
    }

    cyrus::limiter::TokenBucket global_bucket(
        static_cast<double>(cfg_.rate_limit_capacity),
        static_cast<double>(cfg_.rate_limit_refill_per_sec));

    unsigned num_threads = std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 4;

    // 使用 scheduler 组件取代内联队列 + 手工线程管理
    cyrus::scheduler::TaskQueue task_queue(
        static_cast<std::size_t>(cfg_.queue_max_size),
        cfg_.queue_timeout_ms);

    cyrus::scheduler::WorkerPool pool(
        task_queue,
        [&](int fd, int queue_wait_ms) {
            handle_connection(fd, queue_wait_ms, cfg_, global_bucket);
        });
    pool.start(num_threads);

    {
        std::ostringstream oss;
        oss << "epoll listening " << cfg_.listen_host << ":"
            << cfg_.listen_port << " threads=" << num_threads
            << " queue_max=" << cfg_.queue_max_size;
        log_startup(oss.str());
    }

    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    for (;;) {
        const int n = ::epoll_wait(epoll_fd, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != listen_fd) continue;
            for (;;) {
                const int client_fd = ::accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                if (!task_queue.push(client_fd)) {
                    // 队列满，直接拒绝（503 Service Unavailable）
                    auto resp = http::format_json_response(
                        503, R"({"error":"queue_full","message":"server overloaded"})");
                    (void)send_all(client_fd, resp);
                    ::shutdown(client_fd, SHUT_RDWR);
                    ::close(client_fd);
                }
            }
        }
    }

    pool.stop();
    ::close(epoll_fd);
    ::close(listen_fd);
}

}  // namespace cyrus::net
