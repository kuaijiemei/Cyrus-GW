#include <iostream>

namespace cyrus::net {

// 占位：主实现 io_uring / 对照 epoll 事件循环将在此落地。
void event_loop_placeholder() {
    std::cerr << "event_loop: placeholder (see TECH_DESIGN §7)\n";
}

}  // namespace cyrus::net
