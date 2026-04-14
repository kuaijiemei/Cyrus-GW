// ---------------------------------------------------------------------------
// io_uring + C++20 协程 HTTP 服务（主实现）。
// 架构：N 个 worker 线程各持有独立 io_uring ring，共享同一 listen fd。
// 每条连接表示为一个 DetachedTask 协程：
//   co_await async_accept → co_await async_recv → 业务处理 → blocking send → close
// 协程在 co_await 时挂起，由该线程的 event_loop 在 CQE 到达后恢复。
//
// 关键分支原因：
//   - 上游 handle_post_chat 仍为阻塞调用（MVP 限制），因此每线程同一时刻仅有一个
//     协程在执行业务逻辑，其余协程挂起在 io_uring SQE 上。多线程确保并发吞吐。
//   - 对客户端的 accept / recv 使用 io_uring 异步操作，减少 syscall 次数和
//     用户态/内核态切换开销（Proactor 模型）。
//
// 易踩坑点：
//   - UringOp 生命周期与协程帧绑定，co_await 期间指针有效；co_return 后无残留 SQE。
//   - CQE user_data 必须在 submit 前设好，submit_and_wait 可能立即返回已有 CQE。
// ---------------------------------------------------------------------------
#include "net/iouring_server.h"

#include "api/chat_handler.h"
#include "common/errors.h"
#include "common/logger.h"
#include "limiter/token_bucket.h"
#include "net/http_common.h"
#include "net/uring_compat.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <coroutine>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cyrus::net {
namespace {

// ---- coroutine primitives ----

struct UringOp {
    std::coroutine_handle<> coro;
    int result{0};
};

struct DetachedTask {
    struct promise_type {
        DetachedTask get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
};

class UringAwaiter {
public:
    explicit UringAwaiter(struct io_uring_sqe* sqe) : sqe_(sqe) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        op_.coro = h;
        sqe_->user_data = reinterpret_cast<std::uint64_t>(&op_);
    }

    int await_resume() const noexcept { return op_.result; }

private:
    struct io_uring_sqe* sqe_;
    UringOp op_;
};

// ---- listener setup (与 blocking/epoll 共享同一逻辑) ----

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

// ---- blocking send (用于响应写回，MVP 不对写入做 io_uring 异步) ----

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

// ---- 单条连接协程 ----

DetachedTask handle_connection(UringRing& ring, int client_fd,
                               const GatewayConfigSnapshot& cfg,
                               cyrus::limiter::TokenBucket& bucket,
                               std::chrono::steady_clock::time_point accept_time) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const int queue_wait_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t0 - accept_time).count());

    // 1. 异步读取 HTTP 请求头
    std::string raw;
    char recv_buf[8192];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        auto* sqe = uring_get_sqe(ring);
        if (!sqe) { ::close(client_fd); co_return; }
        uring_prep_recv(sqe, client_fd, recv_buf, sizeof(recv_buf), 0);
        int n = co_await UringAwaiter{sqe};
        if (n <= 0) { ::close(client_fd); co_return; }
        raw.append(recv_buf, static_cast<std::size_t>(n));
        if (raw.size() > 65536) { ::close(client_fd); co_return; }
    }

    const auto sep = raw.find("\r\n\r\n");
    std::string headers = raw.substr(0, sep);
    std::string body    = raw.substr(sep + 4);

    // 读齐 Content-Length 指定的 body
    const std::size_t content_length = http::parse_content_length(headers);
    while (body.size() < content_length) {
        auto* sqe = uring_get_sqe(ring);
        if (!sqe) { ::close(client_fd); co_return; }
        uring_prep_recv(sqe, client_fd, recv_buf, sizeof(recv_buf), 0);
        int n = co_await UringAwaiter{sqe};
        if (n <= 0) { ::close(client_fd); co_return; }
        body.append(recv_buf, static_cast<std::size_t>(n));
        if (body.size() > 1024 * 1024) { ::close(client_fd); co_return; }
    }
    if (body.size() > content_length) body.resize(content_length);

    // 2. 路由与处理（与 blocking server 逻辑一致）
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
        co_return;
    }

    if (http::is_health_get(method, path)) {
        auto resp = http::format_json_response(200, http::format_health_body());
        (void)send_all(client_fd, resp);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request("", ms, 200, "", "/health", false, -1, 0, "", queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        co_return;
    }

    if (path == "/chat" && method != "POST") {
        auto resp = http::format_json_response(405, R"({"error":"method_not_allowed"})");
        (void)send_all(client_fd, resp);
        const int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count());
        log_http_request(request_id, ms, 405, "", "/chat", false, -1, 0, "gateway", queue_wait_ms);
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        co_return;
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
            co_return;
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
        co_return;
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

// ---- accept 循环协程 ----

DetachedTask accept_loop(UringRing& ring, int listen_fd,
                         const GatewayConfigSnapshot& cfg,
                         cyrus::limiter::TokenBucket& bucket) {
    for (;;) {
        auto* sqe = uring_get_sqe(ring);
        if (!sqe) {
            co_await std::suspend_always{};
            continue;
        }
        uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
        int client_fd = co_await UringAwaiter{sqe};
        if (client_fd < 0) continue;
        // accept CQE 到达即记录时间，协程立即分发到 handle_connection
        const auto accept_time = std::chrono::steady_clock::now();
        handle_connection(ring, client_fd, cfg, bucket, accept_time);
    }
}

// ---- 单 worker 线程的 event loop ----

void worker_loop(UringRing& ring, int listen_fd,
                 const GatewayConfigSnapshot& cfg,
                 cyrus::limiter::TokenBucket& bucket) {
    accept_loop(ring, listen_fd, cfg, bucket);

    for (;;) {
        int ret = uring_submit_and_wait(ring, 1);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) break;

        while (auto* cqe = uring_peek_cqe(ring)) {
            auto* op = reinterpret_cast<UringOp*>(cqe->user_data);
            if (op) {
                op->result = cqe->res;
                op->coro.resume();
            }
            uring_cqe_seen(ring);
        }
    }
}

}  // namespace

// ---- public entry ----

// 检查 io_uring 是否被内核 sysctl 禁用（RHEL 9 默认 io_uring_disabled=2）
bool check_iouring_available() {
    UringRing probe;
    if (uring_init(probe, 4)) {
        uring_destroy(probe);
        return true;
    }
    return false;
}

void IoUringServer::run() {
    // 关键分支原因：RHEL 9 默认设置 kernel.io_uring_disabled=2，
    // io_uring_setup 会返回 EPERM；在创建线程前先探测，给出明确修复指引。
    if (!check_iouring_available()) {
        std::cerr << "\n"
            "======================================================\n"
            "  io_uring is DISABLED on this kernel (EPERM).\n"
            "  RHEL 9 default: kernel.io_uring_disabled=2\n"
            "\n"
            "  To enable (requires root):\n"
            "    sudo sysctl -w kernel.io_uring_disabled=0\n"
            "\n"
            "  To persist across reboot:\n"
            "    echo 'kernel.io_uring_disabled=0' | "
                     "sudo tee /etc/sysctl.d/99-iouring.conf\n"
            "    sudo sysctl --system\n"
            "======================================================\n\n";
        log_startup("iouring_disabled_by_kernel");
        return;
    }

    const int listen_fd = create_listener(cfg_);
    if (listen_fd < 0) {
        log_startup("iouring_listen_failed");
        return;
    }

    cyrus::limiter::TokenBucket global_bucket(
        static_cast<double>(cfg_.rate_limit_capacity),
        static_cast<double>(cfg_.rate_limit_refill_per_sec));

    unsigned num_threads = std::thread::hardware_concurrency();
    if (num_threads < 1) num_threads = 4;

    {
        std::ostringstream oss;
        oss << "iouring listening " << cfg_.listen_host << ":"
            << cfg_.listen_port << " threads=" << num_threads;
        log_startup(oss.str());
    }

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (unsigned i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            UringRing ring;
            if (!uring_init(ring, 256)) {
                std::cerr << "iouring_init failed on thread " << i
                          << " errno=" << errno << "\n";
                return;
            }
            worker_loop(ring, listen_fd, cfg_, global_bucket);
            uring_destroy(ring);
        });
    }
    for (auto& t : threads) t.join();
    ::close(listen_fd);
}

}  // namespace cyrus::net
