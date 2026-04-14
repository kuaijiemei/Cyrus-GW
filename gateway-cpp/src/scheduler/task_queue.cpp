// ---------------------------------------------------------------------------
// TaskQueue 实现：线程安全 fd 队列 + queue_wait_ms 统计 + 超时自动取消。
// ---------------------------------------------------------------------------
#include "scheduler/task_queue.h"

#include <unistd.h>

namespace cyrus::scheduler {

TaskQueue::TaskQueue(std::size_t max_size, std::int32_t queue_timeout_ms)
    : max_size_(max_size), queue_timeout_ms_(queue_timeout_ms) {}

bool TaskQueue::push(int fd) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_ || queue_.size() >= max_size_) {
        return false;
    }
    queue_.push(QueueItem{fd, std::chrono::steady_clock::now()});
    cv_.notify_one();
    return true;
}

PopResult TaskQueue::pop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [&] { return !queue_.empty() || shutdown_; });
        if (shutdown_ && queue_.empty()) {
            return PopResult{-1, 0};
        }
        if (queue_.empty()) continue;

        auto item = queue_.front();
        queue_.pop();

        const auto now = std::chrono::steady_clock::now();
        const int wait_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - item.enqueue_time)
                .count());

        // 超时取消：入队过久的 fd 直接丢弃，避免客户端已断开却仍处理
        if (queue_timeout_ms_ > 0 && wait_ms > queue_timeout_ms_) {
            ::close(item.fd);
            continue;
        }
        return PopResult{item.fd, wait_ms};
    }
}

std::size_t TaskQueue::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

void TaskQueue::shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    shutdown_ = true;
    while (!queue_.empty()) {
        ::close(queue_.front().fd);
        queue_.pop();
    }
    cv_.notify_all();
}

}  // namespace cyrus::scheduler
