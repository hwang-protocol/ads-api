// 有界阻塞队列 (MPMC): 满了 push 阻塞 → 背压自然传回生产者
// Java 对应: ArrayBlockingQueue。注意 Java 线程池默认的 LinkedBlockingQueue 是无界的，下游慢会 OOM
#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace common {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_{capacity} {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 阻塞直到有空位；队列已关闭返回 false
    bool push(T item)
    {
        std::unique_lock lock{mtx_};
        not_full_.wait(lock, [&] { return closed_ || q_.size() < capacity_; });
        if (closed_) return false;
        q_.push_back(std::move(item));
        lock.unlock();                 // 先解锁再 notify，被唤醒的线程不用再等锁
        not_empty_.notify_one();
        return true;
    }

    // 不阻塞：满了直接返回 false (调用方决定丢弃 / 降级 / 计数)
    bool try_push(T item)
    {
        {
            std::lock_guard lock{mtx_};
            if (closed_ || q_.size() >= capacity_) return false;
            q_.push_back(std::move(item));
        }
        not_empty_.notify_one();
        return true;
    }

    // 阻塞直到有数据；关闭且已取空时返回 nullopt (消费者据此退出)
    std::optional<T> pop()
    {
        std::unique_lock lock{mtx_};
        not_empty_.wait(lock, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return std::nullopt;
        T item = std::move(q_.front());
        q_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return item;
    }

    // 关闭后: push 失败，pop 取完剩余数据后返回 nullopt
    void close()
    {
        {
            std::lock_guard lock{mtx_};
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard lock{mtx_};
        return q_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mtx_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> q_;
    bool closed_{false};
};

}  // namespace common
