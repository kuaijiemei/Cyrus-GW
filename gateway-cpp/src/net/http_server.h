#pragma once

#include "common/models.h"

namespace cyrus::net {

// 最小阻塞式 HTTP 服务：仅 Week1 骨架健康检查；后续由 io_uring/协程路径替换。
class HttpServer {
 public:
    explicit HttpServer(GatewayConfigSnapshot cfg) : cfg_(std::move(cfg)) {}

    void run();

 private:
    GatewayConfigSnapshot cfg_;
};

}  // namespace cyrus::net
