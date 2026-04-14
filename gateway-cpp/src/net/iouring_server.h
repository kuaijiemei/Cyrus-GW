// ---------------------------------------------------------------------------
// 模块职责：基于 io_uring + C++20 无栈协程的高并发 HTTP 服务（主实现）。
// 对外暴露：IoUringServer 类，由 main --mode=iouring 调用 run()。
// 与 blocking / epoll 实现共享 handle_post_chat、TokenBucket、日志等模块。
// ---------------------------------------------------------------------------
#pragma once

#include "common/models.h"

namespace cyrus::net {

class IoUringServer {
public:
    explicit IoUringServer(GatewayConfigSnapshot cfg) : cfg_(std::move(cfg)) {}
    void run();

private:
    GatewayConfigSnapshot cfg_;
};

}  // namespace cyrus::net
