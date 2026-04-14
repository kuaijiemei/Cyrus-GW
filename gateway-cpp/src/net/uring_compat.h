// ---------------------------------------------------------------------------
// 模块职责：基于 <linux/io_uring.h> 和原始系统调用封装最小 io_uring 操作集。
// 对外暴露：UringRing 结构体 + init/destroy/get_sqe/submit/wait/peek/seen
//           + SQE 准备助手（accept/recv/send/close）。
// 不依赖 liburing-devel，仅需内核头文件，可在任何 Linux 5.6+ 环境编译运行。
// ---------------------------------------------------------------------------
//
// 易踩坑点：
//   1. sq_tail 是共享变量（内核读），必须在 submit 时用 release 语义发布。
//   2. cq_head 是共享变量（内核读），seen 后必须用 release 语义推进。
//   3. IORING_OFF_SQ_RING / IORING_OFF_CQ_RING / IORING_OFF_SQES 是 mmap offset，
//      不是字节偏移；IORING_FEAT_SINGLE_MMAP 表示 SQ/CQ 可用同一次 mmap 映射。
// ---------------------------------------------------------------------------
#pragma once

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace cyrus::net {

// ---- raw syscall wrappers ----

inline int sys_io_uring_setup(unsigned entries, struct io_uring_params* p) {
    return static_cast<int>(syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                              unsigned flags) {
    return static_cast<int>(
        syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                nullptr, 0));
}

// ---- ring structure ----

struct UringRing {
    int ring_fd{-1};

    unsigned* sq_head{};
    unsigned* sq_tail{};
    unsigned* sq_ring_mask{};
    unsigned* sq_ring_entries{};
    unsigned* sq_flags{};
    unsigned* sq_array{};
    struct io_uring_sqe* sqes{};

    unsigned* cq_head{};
    unsigned* cq_tail{};
    unsigned* cq_ring_mask{};
    struct io_uring_cqe* cqes{};

    void* sq_ring_ptr{};
    std::size_t sq_ring_sz{};
    void* cq_ring_ptr{};
    std::size_t cq_ring_sz{};
    void* sqes_ptr{};
    std::size_t sqes_sz{};

    unsigned sq_entries{};
    unsigned sqe_local_tail{};
};

// ---- init / destroy ----

inline bool uring_init(UringRing& r, unsigned entries) {
    struct io_uring_params p {};
    r.ring_fd = sys_io_uring_setup(entries, &p);
    if (r.ring_fd < 0) return false;

    r.sq_entries = p.sq_entries;

    // SQ ring mmap
    r.sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r.sq_ring_ptr =
        ::mmap(nullptr, r.sq_ring_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_SQ_RING);
    if (r.sq_ring_ptr == MAP_FAILED) {
        ::close(r.ring_fd);
        r.ring_fd = -1;
        return false;
    }

    auto* base = static_cast<std::uint8_t*>(r.sq_ring_ptr);
    r.sq_head         = reinterpret_cast<unsigned*>(base + p.sq_off.head);
    r.sq_tail         = reinterpret_cast<unsigned*>(base + p.sq_off.tail);
    r.sq_ring_mask    = reinterpret_cast<unsigned*>(base + p.sq_off.ring_mask);
    r.sq_ring_entries = reinterpret_cast<unsigned*>(base + p.sq_off.ring_entries);
    r.sq_flags        = reinterpret_cast<unsigned*>(base + p.sq_off.flags);
    r.sq_array        = reinterpret_cast<unsigned*>(base + p.sq_off.array);

    // SQE array mmap
    r.sqes_sz  = p.sq_entries * sizeof(struct io_uring_sqe);
    r.sqes_ptr = ::mmap(nullptr, r.sqes_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_SQES);
    if (r.sqes_ptr == MAP_FAILED) {
        ::munmap(r.sq_ring_ptr, r.sq_ring_sz);
        ::close(r.ring_fd);
        r.ring_fd = -1;
        return false;
    }
    r.sqes = static_cast<struct io_uring_sqe*>(r.sqes_ptr);

    // CQ ring mmap（SINGLE_MMAP 时与 SQ 共享一块映射）
    r.cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        r.cq_ring_ptr = r.sq_ring_ptr;
    } else {
        r.cq_ring_ptr =
            ::mmap(nullptr, r.cq_ring_sz, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_CQ_RING);
        if (r.cq_ring_ptr == MAP_FAILED) {
            ::munmap(r.sqes_ptr, r.sqes_sz);
            ::munmap(r.sq_ring_ptr, r.sq_ring_sz);
            ::close(r.ring_fd);
            r.ring_fd = -1;
            return false;
        }
    }

    auto* cq_base = static_cast<std::uint8_t*>(r.cq_ring_ptr);
    r.cq_head      = reinterpret_cast<unsigned*>(cq_base + p.cq_off.head);
    r.cq_tail      = reinterpret_cast<unsigned*>(cq_base + p.cq_off.tail);
    r.cq_ring_mask = reinterpret_cast<unsigned*>(cq_base + p.cq_off.ring_mask);
    r.cqes =
        reinterpret_cast<struct io_uring_cqe*>(cq_base + p.cq_off.cqes);

    r.sqe_local_tail = __atomic_load_n(r.sq_tail, __ATOMIC_RELAXED);
    return true;
}

inline void uring_destroy(UringRing& r) {
    if (r.sqes_ptr && r.sqes_ptr != MAP_FAILED)
        ::munmap(r.sqes_ptr, r.sqes_sz);
    if (r.cq_ring_ptr && r.cq_ring_ptr != MAP_FAILED &&
        r.cq_ring_ptr != r.sq_ring_ptr)
        ::munmap(r.cq_ring_ptr, r.cq_ring_sz);
    if (r.sq_ring_ptr && r.sq_ring_ptr != MAP_FAILED)
        ::munmap(r.sq_ring_ptr, r.sq_ring_sz);
    if (r.ring_fd >= 0) ::close(r.ring_fd);
    r.ring_fd = -1;
}

// ---- SQE 操作 ----

inline struct io_uring_sqe* uring_get_sqe(UringRing& r) {
    unsigned head = __atomic_load_n(r.sq_head, __ATOMIC_ACQUIRE);
    unsigned next = r.sqe_local_tail;
    if (next - head >= r.sq_entries) return nullptr;

    unsigned idx = next & *r.sq_ring_mask;
    r.sq_array[idx] = idx;
    r.sqe_local_tail = next + 1;
    auto* sqe = &r.sqes[idx];
    std::memset(sqe, 0, sizeof(*sqe));
    return sqe;
}

// ---- submit / wait ----

inline int uring_submit(UringRing& r) {
    unsigned tail = r.sqe_local_tail;
    __atomic_store_n(r.sq_tail, tail, __ATOMIC_RELEASE);
    unsigned to_submit =
        tail - __atomic_load_n(r.sq_head, __ATOMIC_ACQUIRE);
    if (to_submit == 0) return 0;
    return sys_io_uring_enter(r.ring_fd, to_submit, 0, 0);
}

inline int uring_submit_and_wait(UringRing& r, unsigned wait_nr) {
    unsigned tail = r.sqe_local_tail;
    __atomic_store_n(r.sq_tail, tail, __ATOMIC_RELEASE);
    unsigned to_submit =
        tail - __atomic_load_n(r.sq_head, __ATOMIC_ACQUIRE);
    return sys_io_uring_enter(r.ring_fd, to_submit, wait_nr,
                              IORING_ENTER_GETEVENTS);
}

// ---- CQE 操作 ----

inline struct io_uring_cqe* uring_peek_cqe(UringRing& r) {
    unsigned head = __atomic_load_n(r.cq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r.cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail) return nullptr;
    return &r.cqes[head & *r.cq_ring_mask];
}

inline void uring_cqe_seen(UringRing& r) {
    unsigned head = *r.cq_head + 1;
    __atomic_store_n(r.cq_head, head, __ATOMIC_RELEASE);
}

// ---- SQE 准备助手 ----

inline void uring_prep_accept(struct io_uring_sqe* sqe, int fd,
                              sockaddr* addr, socklen_t* addrlen,
                              int flags) {
    sqe->opcode       = IORING_OP_ACCEPT;
    sqe->fd           = fd;
    sqe->addr         = reinterpret_cast<std::uint64_t>(addr);
    sqe->addr2        = reinterpret_cast<std::uint64_t>(addrlen);
    sqe->accept_flags = static_cast<std::uint32_t>(flags);
}

inline void uring_prep_recv(struct io_uring_sqe* sqe, int fd, void* buf,
                            unsigned len, int flags) {
    sqe->opcode    = IORING_OP_RECV;
    sqe->fd        = fd;
    sqe->addr      = reinterpret_cast<std::uint64_t>(buf);
    sqe->len       = len;
    sqe->msg_flags = static_cast<std::uint32_t>(flags);
}

inline void uring_prep_send(struct io_uring_sqe* sqe, int fd, const void* buf,
                            unsigned len, int flags) {
    sqe->opcode    = IORING_OP_SEND;
    sqe->fd        = fd;
    sqe->addr      = reinterpret_cast<std::uint64_t>(buf);
    sqe->len       = len;
    sqe->msg_flags = static_cast<std::uint32_t>(flags);
}

inline void uring_prep_close(struct io_uring_sqe* sqe, int fd) {
    sqe->opcode = IORING_OP_CLOSE;
    sqe->fd     = fd;
}

}  // namespace cyrus::net
