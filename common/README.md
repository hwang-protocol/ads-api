# common —— 共用的 header-only 库

| 文件 | 内容 | Java 对应 | 用在 |
|---|---|---|---|
| `bounded_queue.h` | 有界阻塞队列 (MPMC)，满了阻塞生产者 = 背压；`close()` 后消费者取完剩余数据再退出 | `ArrayBlockingQueue` | 线程池、p4 审批队列 |
| `thread_pool.h` | 固定线程数 + 有界任务队列，`submit` 返回 `std::future`，析构时优雅关闭 | `ThreadPoolExecutor` + `CallerRunsPolicy` | 测试 |
| `spsc_ring.h` | 无锁单生产者单消费者环形缓冲；head/tail 放在不同 cache line；acquire/release | LMAX Disruptor / JCTools | p2 网关 → 分片 |
| `latency_histogram.h` | 对数分桶直方图，固定内存，O(1) 记录，误差 < 6.25%；可以合并 | HdrHistogram | p1、p2 |
| `sqlite.h` | `Db` / `Stmt` / `Transaction` 的 RAII 封装 (`unique_ptr` + 自定义 deleter)；事务对象析构时如果还没 commit 就自动回滚 | JDBC + try-with-resources | p1、p3、p5 |
| `cli.h` | `--key value` 参数解析 | | 全部 |
| `check.h` | 极简测试宏 (本机没装 GoogleTest) | JUnit `assert*` | 全部测试 |

测试：`tests/common_tests.cpp` (多生产者多消费者求和、SPSC 保序、线程池、直方图精度、事务回滚)。
