// ---------------------------------------------------------------------------
// 模块职责：基于 epoll + 线程池的 HTTP 服务（对照实现 / 性能压测对比用）。
// 对外暴露：EpollServer 类，由 main --mode=epoll 调用 run()。
// 设计要点：主线程 epoll_wait 仅负责 accept 新连接，连接处理分发到固定线程池中
//           的 worker 以阻塞 I/O 完成读-处理-写全流程（Reactor 调度 + 同步执行）。
// ---------------------------------------------------------------------------
#pragma once

#include "common/models.h"

namespace cyrus::net {

class EpollServer {
public:
    explicit EpollServer(GatewayConfigSnapshot cfg) : cfg_(std::move(cfg)) {}
    void run();

private:
    GatewayConfigSnapshot cfg_;
};

}  // namespace cyrus::net
