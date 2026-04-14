#pragma once

#include <chrono>
#include <mutex>

namespace cyrus::limiter {

// 模块职责：单机令牌桶限流（MVP 用全局桶），用于 /chat 快速拒绝超限流量。
class TokenBucket {
 public:
    TokenBucket(double capacity, double refill_per_sec);

    // 关键分支原因：返回 false 代表立即拒绝（429），避免请求继续占用上游与 CPU。
    bool try_consume(double tokens = 1.0);

 private:
    void refill_locked(const std::chrono::steady_clock::time_point now);

    double capacity_;
    double refill_per_sec_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    std::mutex mu_;
};

}  // namespace cyrus::limiter
