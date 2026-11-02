// 极简测试宏 (本机没装 GoogleTest，接口故意写得像 gtest，迁移时直接替换)
#pragma once
#include <cstdlib>
#include <iostream>

namespace common::test {
inline int& failures()
{
    static int n = 0;
    return n;
}
}  // namespace common::test

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";    \
            ++common::test::failures();                                                   \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        /* 按值拷贝: auto&& 会绑到临时对象的成员上 (如 cli.Get(...)->status)，语句结束就悬空 */ \
        const auto va_ = (a);                                                             \
        const auto vb_ = (b);                                                             \
        if (!(va_ == vb_)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " = "    \
                      << va_ << ", " #b " = " << vb_ << "\n";                             \
            ++common::test::failures();                                                   \
        }                                                                                 \
    } while (0)

#define TEST_MAIN_RESULT()                                                                \
    (common::test::failures() == 0 ? (std::cout << "ALL PASSED\n", EXIT_SUCCESS)          \
                                   : (std::cout << common::test::failures() << " FAILED\n", EXIT_FAILURE))
