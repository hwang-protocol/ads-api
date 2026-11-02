// 固定大小线程池 + 有界任务队列
// Java 对应: new ThreadPoolExecutor(n, n, 0, ms, new ArrayBlockingQueue<>(cap), CallerRunsPolicy)
// 区别: C++ 标准库没有线程池，submit 返回 std::future (≈ Future.get())
#pragma once
#include "common/bounded_queue.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

namespace common {

class ThreadPool {
public:
    ThreadPool(std::size_t num_threads, std::size_t queue_capacity) : tasks_{queue_capacity}
    {
        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back([this] {
                while (auto task = tasks_.pop()) (*task)();
            });
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 析构 = 优雅关闭: 不再接新任务，已入队的任务全部执行完 (≈ shutdown() + awaitTermination())
    ~ThreadPool()
    {
        tasks_.close();
        // jthread 析构时自动 join
    }

    // 队列满时阻塞调用者 → 背压
    template <typename F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        // packaged_task 不可拷贝，std::function 要求可拷贝 → 用 shared_ptr 包一层
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        if (!tasks_.push([task] { (*task)(); }))
            throw std::runtime_error("ThreadPool is shut down");
        return fut;
    }

    std::size_t size() const { return workers_.size(); }

private:
    BoundedQueue<std::function<void()>> tasks_;   // 必须在 workers_ 之前声明: 析构顺序相反
    std::vector<std::jthread> workers_;
};

}  // namespace common
