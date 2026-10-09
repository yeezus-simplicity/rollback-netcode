// concurrent.h — 无锁并发原语（无任何第三方依赖）
//
// 为 server 的网络 I/O 并发化提供两套原语：
//   MpscQueue<T> : 多生产者 / 单消费者队列
//                  场景——多个网络 worker 线程把解析出的网络事件
//                  入队，模拟线程（唯一消费者）每 tick 一次性 drain 后
//                  在单线程内处理（保证确定性模拟不被并发破坏）。
//   SpmcRing<T,N>: 单生产者 / 单消费者定容环（N 为 2 的幂）
//                  场景——模拟线程按连接把状态包写入该连接的发送环，
//                  专属 sender worker 负责实际 send。环满即背压触发，
//                  调用方丢弃最旧包并计数，慢客户端不会阻塞快客户端/tick。
//
// memory_order 严格配对（acquire/release、exchange acq_rel），
// 可直接通过 ThreadSanitizer 验证无数据竞争。
//
// 设计取舍：低频率路径（连接建立/销毁、worker 列表维护）允许用 std::mutex，
//           只有每包收发的热路径走无锁原语。

#pragma once

#include <atomic>
#include <cstddef>
#include <new>

namespace synq {
namespace net {

// ======================= MPSC 队列（多生产者单消费者）=======================
//
// 经典 Dmitry Vyukov MPSC 链表算法：
//   enqueue: 一次 exchange（acq_rel）即可，多生产者安全，无 CAS 循环。
//   dequeue: 仅单消费者调用，无 CAS。
// 节点由生产者 new、消费者 delete（输入事件频率远低于微秒级，堆分配可接受）。

template <typename T>
class MpscQueue {
  struct Node {
    std::atomic<Node*> next;
    T value;
    Node() : next(nullptr) {}
  };

  std::atomic<Node*> head_;  // 生产者侧：最新入队的节点
  std::atomic<Node*> tail_;  // 消费者侧：下一个待出队节点
  Node* stub_;               // 空队列哨兵

 public:
  MpscQueue() {
    stub_ = new Node();
    stub_->next.store(nullptr, std::memory_order_relaxed);
    head_.store(stub_, std::memory_order_relaxed);
    tail_.store(stub_, std::memory_order_relaxed);
  }

  ~MpscQueue() {
    T tmp;
    while (dequeue(tmp)) { /* 释放节点 */ }
    delete stub_;
  }

  // 生产者调用（可被多个线程同时调用；按值接收，支持左值/右值）
  void enqueue(T v) {
    Node* n = new Node();
    n->value = std::move(v);
    n->next.store(nullptr, std::memory_order_relaxed);
    Node* prev = head_.exchange(n, std::memory_order_acq_rel);
    prev->next.store(n, std::memory_order_release);
  }

  // 消费者调用（必须仅由一个线程调用）
  bool dequeue(T& out) {
    Node* tail = tail_.load(std::memory_order_relaxed);
    Node* next = tail->next.load(std::memory_order_acquire);
    if (next == nullptr) return false;  // 队列空
    tail_.store(next, std::memory_order_relaxed);
    out = std::move(next->value);
    if (tail == stub_) {
      // 弹出的节点原本是哨兵：把它升级为新哨兵，释放旧哨兵内存
      delete stub_;
      stub_ = next;
    } else {
      delete tail;
    }
    return true;
  }
};

// ======================= SPSC 环（单生产者单消费者）=======================
//
// 定容环，容量为 N（必须是 2 的幂），用单调递增的序列号 + 位掩码取模。
// 生产者与消费者各只有唯一一个线程访问，因此只需 release/acquire 配对即可，
// 无需 CAS。try_push 在满时返回 false —— 这是背压的触发点。

template <typename T, std::size_t N>
class SpmcRing {
  static_assert(N > 0 && (N & (N - 1)) == 0, "N 必须是 2 的幂");

  T buf_[N];
  std::atomic<std::size_t> head_{0};  // 生产者写位置（单调递增）
  std::atomic<std::size_t> tail_{0};  // 消费者读位置（单调递增）

 public:
  // 生产者（唯一线程）调用
  bool try_push(const T& v) {
    std::size_t h = head_.load(std::memory_order_relaxed);
    std::size_t t = tail_.load(std::memory_order_acquire);
    if (h - t == N) return false;  // 满 —— 背压触发
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // 消费者（唯一线程）调用
  bool try_pop(T& out) {
    std::size_t t = tail_.load(std::memory_order_relaxed);
    std::size_t h = head_.load(std::memory_order_acquire);
    if (t == h) return false;  // 空
    out = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  std::size_t size() const {
    return head_.load(std::memory_order_relaxed) -
           tail_.load(std::memory_order_relaxed);
  }
  bool full() const { return size() >= N; }
};

}  // namespace net
}  // namespace synq
