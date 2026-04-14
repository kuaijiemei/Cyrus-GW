// ---------------------------------------------------------------------------
// 模块职责：线程安全的 fd 任务队列，供 epoll 等 Reactor 模式使用。
// 对外暴露：TaskQueue 类（push / pop / size / shutdown）。
//
// 关键设计：
//   - 每个 fd 入队时记录时间戳，出队时计算 queue_wait_ms。
//   - 支持 max_size 限制：队列满时 push 返回 false（调用方决定 503 or 丢弃）。
//   - 支持超时取消：pop 时自动跳过入队超过 queue_timeout_ms 的任务（直接 close fd）。
//   - shutdown() 后唤醒所有 worker，清空队列并关闭残留 fd。
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>

namespace cyrus::scheduler {

struct QueueItem {
    int fd{-1};
    std::chrono::steady_clock::time_point enqueue_time{};
};

struct PopResult {
    int fd{-1};
    int queue_wait_ms{0};
};

class TaskQueue {
public:
    explicit TaskQueue(std::size_t max_size = 2000,
                       std::int32_t queue_timeout_ms = 0);

    // 入队：成功返回 true，队列满或已 shutdown 返回 false。
    bool push(int fd);

    // 阻塞出队：返回 fd 和 queue_wait_ms。shutdown 后队列空则返回 fd=-1。
    PopResult pop();

    // 当前队列深度（近似值，仅统计用）。
    std::size_t size() const;

    // 优雅关闭：唤醒所有等待线程，清空残留 fd。
    void shutdown();

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::queue<QueueItem> queue_;

    std::size_t max_size_;
    std::int32_t queue_timeout_ms_;
    bool shutdown_{false};
};

}  // namespace cyrus::scheduler
