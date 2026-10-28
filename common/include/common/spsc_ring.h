// 无锁单生产者单消费者环形缓冲 (SPSC)
// 交易系统热路径的标准结构: 行情线程 → 策略线程、网关线程 → 风控线程
// Java 对应: LMAX Disruptor / JCTools SpscArrayQueue
#pragma once
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <optional>
#include <vector>

namespace common {

template <typename T>
class SpscRing {
public:
    // 容量取 2 的幂，下标用 & mask 代替 % (除法很慢)
    explicit SpscRing(std::size_t capacity_pow2) : buf_(capacity_pow2), mask_{capacity_pow2 - 1}
    {
        if (capacity_pow2 == 0 || (capacity_pow2 & mask_) != 0)
            throw std::invalid_argument("capacity must be a power of two");
    }

    // 只能由生产者线程调用
    bool try_push(const T& v)
    {
        const auto head = head_.load(std::memory_order_relaxed);   // 自己写的，relaxed 即可
        if (head - tail_cache_ == buf_.size()) {                   // 看起来满了，再读一次真实 tail
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head - tail_cache_ == buf_.size()) return false;
        }
        buf_[head & mask_] = v;
        head_.store(head + 1, std::memory_order_release);          // release: 数据写入对消费者可见
        return true;
    }

    // 只能由消费者线程调用
    std::optional<T> try_pop()
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);   // acquire: 与生产者的 release 配对
            if (tail == head_cache_) return std::nullopt;
        }
        T v = buf_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return v;
    }

private:
    // head/tail 分别在不同 cache line，避免 false sharing (两个核来回抢同一行缓存)
    static constexpr std::size_t kCacheLine = 64;
    std::vector<T> buf_;
    const std::size_t mask_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::size_t tail_cache_{0};   // 生产者缓存的 tail，减少跨核读取
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::size_t head_cache_{0};   // 消费者缓存的 head
};

}  // namespace common
