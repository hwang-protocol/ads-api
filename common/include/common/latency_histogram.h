// 延迟直方图 (HdrHistogram 的简化版): 固定内存、O(1) 记录，可算 p50/p99/p999
// 为什么不存所有样本再排序: 线上每秒百万次记录，存样本内存和排序都扛不住
// 分桶: 每个 2 的幂区间再均分 16 个子桶 → 相对误差 < 1/16 ≈ 6%
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace common {

inline std::int64_t now_ns()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// 墙钟 (跨进程比较时间戳用，比如行情源时间 vs 本地落库时间)
inline std::int64_t wall_ns()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

class LatencyHistogram {
public:
    void record(std::int64_t ns)
    {
        if (ns < 0) ns = 0;
        ++buckets_[index_of(static_cast<std::uint64_t>(ns))];
        ++count_;
        max_ = std::max(max_, ns);
    }

    void merge(const LatencyHistogram& other)
    {
        for (std::size_t i = 0; i < kBuckets; ++i) buckets_[i] += other.buckets_[i];
        count_ += other.count_;
        max_ = std::max(max_, other.max_);
    }

    // p 取 0~100，返回该分位所在桶的上界 (偏保守)
    std::int64_t percentile(double p) const
    {
        if (count_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(std::max(1.0, p / 100.0 * static_cast<double>(count_)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += buckets_[i];
            if (seen >= target) return std::min<std::int64_t>(upper_bound_of(i), max_);
        }
        return max_;
    }

    std::uint64_t count() const { return count_; }
    std::int64_t max() const { return max_; }

    std::string summary(std::string_view unit = "us") const
    {
        const double div = unit == "ms" ? 1e6 : unit == "ns" ? 1.0 : 1e3;
        return std::format("n={} p50={:.1f}{} p99={:.1f}{} p99.9={:.1f}{} max={:.1f}{}", count_,
                           percentile(50) / div, unit, percentile(99) / div, unit,
                           percentile(99.9) / div, unit, max_ / div, unit);
    }

private:
    static constexpr int kSubBits = 4;                     // 16 个子桶
    static constexpr std::size_t kSub = 1u << kSubBits;
    static constexpr std::size_t kBuckets = 64 * kSub;

    static std::size_t index_of(std::uint64_t v)
    {
        if (v < kSub) return static_cast<std::size_t>(v);  // 小值直接精确记录
        const int msb = 63 - std::countl_zero(v);          // 最高位所在位置
        const auto sub = (v >> (msb - kSubBits)) & (kSub - 1);
        return static_cast<std::size_t>(msb - kSubBits + 1) * kSub + sub;
    }

    static std::int64_t upper_bound_of(std::size_t idx)
    {
        if (idx < kSub) return static_cast<std::int64_t>(idx);
        const auto group = idx / kSub;                     // = msb - kSubBits + 1
        const auto sub = idx % kSub;
        const int msb = static_cast<int>(group) + kSubBits - 1;
        const std::uint64_t base = (kSub + sub) << (msb - kSubBits);
        return static_cast<std::int64_t>(base + (1ull << (msb - kSubBits)) - 1);
    }

    std::array<std::uint64_t, kBuckets> buckets_{};
    std::uint64_t count_{0};
    std::int64_t max_{0};
};

}  // namespace common
