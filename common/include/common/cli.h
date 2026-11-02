// 极简命令行参数: --key value / --flag
#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace common {

class Args {
public:
    Args(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            if (!a.starts_with("--")) throw std::invalid_argument("unexpected argument: " + std::string(a));
            if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"))
                kv_[std::string(a)] = argv[++i];
            else
                kv_[std::string(a)] = "";   // 布尔开关
        }
    }

    bool has(const std::string& k) const { return kv_.contains(k); }

    std::string str(const std::string& k, std::string def) const
    {
        auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }

    template <typename T>
    T num(const std::string& k, T def) const
    {
        auto it = kv_.find(k);
        if (it == kv_.end()) return def;
        T v{};
        const auto& s = it->second;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size()) throw std::invalid_argument("bad value for " + k + ": " + s);
        return v;
    }

private:
    std::unordered_map<std::string, std::string> kv_;
};

}  // namespace common
