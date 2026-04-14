#include "limiter/token_bucket.h"

#include <algorithm>

namespace cyrus::limiter {

TokenBucket::TokenBucket(double capacity, double refill_per_sec)
    : capacity_(std::max(capacity, 1.0)),
      refill_per_sec_(std::max(refill_per_sec, 0.0)),
      tokens_(std::max(capacity, 1.0)),
      last_refill_(std::chrono::steady_clock::now()) {}

bool TokenBucket::try_consume(double tokens) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    refill_locked(now);
    if (tokens_ < tokens) {
        return false;
    }
    tokens_ -= tokens;
    return true;
}

void TokenBucket::refill_locked(const std::chrono::steady_clock::time_point now) {
    if (refill_per_sec_ <= 0.0) {
        last_refill_ = now;
        return;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refill_).count();
    if (elapsed_ms <= 0) {
        return;
    }
    const double add = static_cast<double>(elapsed_ms) * refill_per_sec_ / 1000.0;
    tokens_ = std::min(capacity_, tokens_ + add);
    last_refill_ = now;
}

}  // namespace cyrus::limiter
