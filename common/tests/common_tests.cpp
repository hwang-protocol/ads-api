// common 库的单元测试 (建议用 -DSANITIZE=thread 再跑一遍)
#include "common/bounded_queue.h"
#include "common/check.h"
#include "common/latency_histogram.h"
#include "common/spsc_ring.h"
#include "common/sqlite.h"
#include "common/thread_pool.h"

#include <atomic>
#include <numeric>

static void test_bounded_queue_mpmc()
{
    common::BoundedQueue<int> q{8};   // 容量很小，逼生产者频繁阻塞
    constexpr int kPerProducer = 10'000;
    std::atomic<long> sum{0};
    {
        std::vector<std::jthread> consumers;
        for (int c = 0; c < 3; ++c)
            consumers.emplace_back([&] {
                while (auto v = q.pop()) sum += *v;
            });
        {
            std::vector<std::jthread> producers;
            for (int p = 0; p < 4; ++p)
                producers.emplace_back([&] {
                    for (int i = 1; i <= kPerProducer; ++i) q.push(i);
                });
        }   // 生产者全部 join
        q.close();
    }       // 消费者取完后退出并 join
    CHECK_EQ(sum.load(), 4L * kPerProducer * (kPerProducer + 1) / 2);
}

static void test_spsc_ring_order()
{
    common::SpscRing<int> ring{1024};
    constexpr int kN = 200'000;
    std::jthread producer([&] {
        for (int i = 0; i < kN; ++i)
            while (!ring.try_push(i)) {}
    });
    int expected = 0;
    bool in_order = true;
    while (expected < kN) {
        if (auto v = ring.try_pop()) {
            in_order &= (*v == expected);
            ++expected;
        }
    }
    CHECK(in_order);   // SPSC 保证 FIFO 顺序
}

static void test_thread_pool()
{
    common::ThreadPool pool{4, 16};
    std::vector<std::future<int>> futs;
    for (int i = 0; i < 100; ++i) futs.push_back(pool.submit([i] { return i * i; }));
    int total = 0;
    for (auto& f : futs) total += f.get();
    CHECK_EQ(total, 328350);   // sum of i^2, i = 0..99
}

static void test_histogram()
{
    common::LatencyHistogram h;
    for (int i = 1; i <= 1000; ++i) h.record(i * 1000);   // 1us .. 1000us
    const auto p50 = h.percentile(50);
    const auto p99 = h.percentile(99);
    CHECK(p50 >= 500'000 && p50 <= 500'000 * 1.07);       // 分桶相对误差 < 6.25%
    CHECK(p99 >= 990'000 && p99 <= 990'000 * 1.07);
    CHECK_EQ(h.percentile(100), 1'000'000);
}

static void test_sqlite_txn_rollback()
{
    common::Db db{":memory:"};
    db.exec("CREATE TABLE t(x INTEGER)");
    try {
        common::Transaction txn{db};
        db.exec("INSERT INTO t VALUES (1)");
        throw std::runtime_error("boom");   // 没 commit 就离开作用域
    } catch (const std::runtime_error&) {
    }
    CHECK_EQ(db.scalar_int("SELECT COUNT(*) FROM t"), 0);   // 已回滚
    {
        common::Transaction txn{db};
        auto ins = db.prepare("INSERT INTO t VALUES (?)");
        for (int i = 0; i < 10; ++i) ins.bind(1, i).exec_and_reset();
        txn.commit();
    }
    CHECK_EQ(db.scalar_int("SELECT SUM(x) FROM t"), 45);
}

int main()
{
    test_bounded_queue_mpmc();
    test_spsc_ring_order();
    test_thread_pool();
    test_histogram();
    test_sqlite_txn_rollback();
    return TEST_MAIN_RESULT();
}
