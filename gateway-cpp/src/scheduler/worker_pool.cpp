// ---------------------------------------------------------------------------
// WorkerPool 实现：N 个线程从 TaskQueue 取 fd，执行 handler(fd, queue_wait_ms)。
// ---------------------------------------------------------------------------
#include "scheduler/worker_pool.h"

namespace cyrus::scheduler {

WorkerPool::WorkerPool(TaskQueue& queue, Handler handler)
    : queue_(queue), handler_(std::move(handler)) {}

void WorkerPool::start(unsigned num_threads) {
    if (started_) return;
    started_ = true;

    workers_.reserve(num_threads);
    for (unsigned i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            for (;;) {
                auto item = queue_.pop();
                if (item.fd < 0) return;
                handler_(item.fd, item.queue_wait_ms);
            }
        });
    }
}

void WorkerPool::stop() {
    if (!started_) return;
    queue_.shutdown();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    started_ = false;
}

WorkerPool::~WorkerPool() {
    stop();
}

}  // namespace cyrus::scheduler
