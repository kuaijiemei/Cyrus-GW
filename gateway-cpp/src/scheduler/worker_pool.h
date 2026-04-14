// ---------------------------------------------------------------------------
// 模块职责：固定大小的 worker 线程池，从 TaskQueue 取 fd 并执行处理函数。
// 对外暴露：WorkerPool 类（start / stop）。
//
// 使用方式：
//   1. 构造时传入 TaskQueue 引用和 handler（签名 void(int fd, int queue_wait_ms)）。
//   2. start(n) 启动 n 个 worker。
//   3. stop() 先调用 TaskQueue::shutdown()，再 join 所有线程。
// ---------------------------------------------------------------------------
#pragma once

#include "scheduler/task_queue.h"

#include <functional>
#include <thread>
#include <vector>

namespace cyrus::scheduler {

class WorkerPool {
public:
    using Handler = std::function<void(int fd, int queue_wait_ms)>;

    WorkerPool(TaskQueue& queue, Handler handler);

    void start(unsigned num_threads);
    void stop();

    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

private:
    TaskQueue& queue_;
    Handler handler_;
    std::vector<std::thread> workers_;
    bool started_{false};
};

}  // namespace cyrus::scheduler
