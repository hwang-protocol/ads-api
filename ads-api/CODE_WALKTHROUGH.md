# 源码导读（写给刚学 C++ 的人）

建议把这份笔记和代码并排打开，一边看一边对照。每个例子都摘自真实代码，链接可以直接点过去。

---

## 0. 先认识目录：`.h` 和 `.cpp`

```
include/ads/*.h   头文件: "有什么" —— 类型、函数的签名 (声明)
src/*.cpp         源文件: "怎么做" —— 函数体 (定义)
```

Java 里一个类的声明和实现写在同一个 `.java` 文件里。C++ 通常拆成两部分：
- **`.h`** 给别人 `#include`，相当于 Java 的 `interface` 加上类的公开签名。
- **`.cpp`** 是具体实现，只编译一次。

所以**读代码时先看 `.h`**：它告诉你有哪些类型、能调用哪些函数；需要知道细节时再去看 `.cpp`。

推荐阅读顺序（由浅到深）：

| 顺序 | 文件 | 内容 |
|---|---|---|
| 1 | [model.h](include/ads/model.h) / [model.cpp](src/model.cpp) | 数据结构：Campaign、Keyword、点击事件 |
| 2 | [stores.h](include/ads/stores.h) | 三个存储**接口**（只有签名，没有实现） |
| 3 | [memory_store.cpp](src/memory_store.cpp) | 接口最简单的实现：全放在内存里 |
| 4 | [budget_service.h](include/ads/budget_service.h) / [.cpp](src/budget_service.cpp) | **核心业务逻辑**：预算租约 |
| 5 | [api.cpp](src/api.cpp) | HTTP 路由：请求进来先到这里 |
| 6 | [main.cpp](src/main.cpp) | 程序入口：把所有对象组装起来 |
| 7 | [dynamo_store.cpp](src/dynamo_store.cpp) / [pg_ledger.cpp](src/pg_ledger.cpp) | 真实数据库的实现（细节多，最后看） |

---

## 1. 怎么读一个函数签名

一个完整的 C++ 函数签名长这样（方括号里的部分是可选的）：

```
[virtual] 返回类型 [类名::]函数名(参数列表) [const] [override | final | = 0 | = default | = delete]
```

拿真实代码里最长的一个来拆，[budget_service.h:88](include/ads/budget_service.h#L88)：

```cpp
std::expected<ChargeResult, StoreError> charge(const std::string& advertiser_id,
                                               const std::string& campaign_id,
                                               std::span<const ChargeEvent> events);
```

| 部分 | 含义 |
|---|---|
| `std::expected<ChargeResult, StoreError>` | **返回类型**：要么是一个 `ChargeResult`（成功），要么是一个 `StoreError`（失败） |
| `charge` | 函数名 |
| `const std::string& advertiser_id` | 参数 1：只读地借用调用方的字符串，不拷贝 |
| `const std::string& campaign_id` | 参数 2：同上 |
| `std::span<const ChargeEvent> events` | 参数 3：一组点击事件的"视图"，不拷贝，也不能修改 |

翻译成 Java 大概是 `Result<ChargeResult, StoreError> charge(String advertiserId, String campaignId, List<ChargeEvent> events)`。区别在于 C++ **要你明确写出每个参数是拷贝、借用还是可修改**，Java 不用写，是因为对象一律按引用传递。

### 1.1 参数的几种写法（最重要的一张表）

| 写法 | 意思 | 什么时候用 | 代码里的例子 |
|---|---|---|---|
| `int x`、`Micros m`、`double d` | **按值传递**：拷贝一份，函数里改了也不影响调用方 | 小类型（数字、bool、enum），拷贝比传引用还便宜 | `validate_budget(Micros m)` |
| `const std::string& s` | **const 引用**：借用调用方的对象，不拷贝，**不能改** | 大对象（string、vector、struct）只读的时候。**最常见的写法** | `get(const std::string& advertiser_id, ...)` |
| `std::string_view s` | **字符串视图**：只记住"字符从哪开始、有多长"，不拷贝、不能改，按值传递 | 只读字符串，而且希望能直接传 `"ENABLED"` 这样的字面量，不用先构造一个 `std::string` | `parse_status(std::string_view s)` |
| `Lease& l` | **非 const 引用**：借用调用方的对象，**会修改它** | 函数的作用就是修改传进来的对象 | `roll_over(Lease& l, ...)` |
| `httplib::Response& res` | 同上，这种也叫**输出参数** | 函数把结果写进去 | `reply(httplib::Response& res, int status, const json& body)` |
| `std::span<const ChargeEvent> events` | **数组视图**：指向一段连续的元素（`vector` 或数组都行），不拷贝 | 只读一组元素，不关心调用方用的是 `vector` 还是数组 | `charge(..., std::span<const ChargeEvent> events)` |
| `BudgetConfig cfg = {}` | **默认参数**：调用时可以不传，不传就用 `{}`（全部字段取默认值） | 可选的配置 | `BudgetService(..., BudgetConfig cfg = {})` |
| `const char* key` | C 风格的字符串指针 | 和 C 库打交道，或者参数只会是字面量 | `get_int(const json& j, const char* key)` |
| `PGconn* c` | **裸指针**：指向一个对象，但**不负责释放它** | C 库（libpq）给的句柄 | `exec(PGconn* c, ...)` |
| `std::initializer_list<std::string> params` | 让调用方可以直接写 `{a, b, c}` | 参数个数不固定 | `exec(conn, sql, {campaign_id, date})` |

**判断规则**（初学时这样记就够了）：
1. 数字、bool、enum → **按值**。
2. 字符串、vector、struct，只读 → **`const T&`**（只读字符串也可以用 `string_view`）。
3. 要修改调用方的对象 → **`T&`**。
4. 一组元素，只读 → **`std::span<const T>`**。

**为什么 Java 没有这些？** Java 的对象一律按引用传递，相当于 C++ 的 `T&`，"不拷贝"和"不被修改"都没法从签名上看出来。C++ 默认是**拷贝**，如果把 `const std::string& s` 写成 `std::string s`，每次调用都会复制一遍字符串。在热路径上（比如每次点击都要调用的 `charge`）这就是性能问题。

### 1.2 返回值的几种写法

| 写法 | 意思 | 调用方怎么用 | 代码里的例子 |
|---|---|---|---|
| `void` | 不返回东西 | — | `put_keyword(...)` |
| `std::optional<Campaign>` | **可能没有**结果（≈ Java 的 `Optional`） | `if (!c)` 判断有没有；`*c` 或 `c->name` 取值；`c.value_or(默认值)` | `get(...)`：找不到就返回 `std::nullopt` |
| `std::expected<T, E>` | **要么成功（T），要么失败（E）**（≈ Rust 的 `Result`） | `if (!r)` 判断失败；`r.error()` 取出错误；`r->xxx` 取成功的值 | `create(...)`、`update(...)`、`charge(...)` |
| `std::vector<Campaign>` | 返回一个新的数组 | 直接用 | `list(...)` |
| `Grant`（一个小 struct） | 一次返回多个值 | `g.granted`、`g.remaining_after` | `lease(...)` |
| `std::string_view` | 返回一个只读视图 | 直接用 | `to_string(CampaignStatus)` 返回 `"ENABLED"` 这样的字面量 |
| `Lease&` | **返回引用**：返回的就是 map 里那个对象本身，不是拷贝 | 拿到后可以直接修改它 | `lease_for(...)` |

两个常见问题：

- **返回 `std::vector` 会不会拷贝一遍，很慢？** 不会。编译器会直接在调用方的位置构造这个对象（返回值优化 RVO），或者把内部缓冲区"移动"过去（move），都不会复制元素。所以放心按值返回。
- **什么时候不能返回引用？** 不能返回函数里局部变量的引用，因为函数结束后变量就被销毁了，引用会变成悬空引用（dangling）。`lease_for` 能返回引用，是因为它指向的对象存在 `leases_` 这个成员 map 里，函数返回后还活着。
  - 返回 `string_view` 也是同样的道理：`to_string` 返回的 `"ENABLED"` 是字面量，存在程序的只读数据区，程序运行期间一直有效，所以安全。

`optional` 和 `expected` 的用法，在 [budget_service.cpp:83](src/budget_service.cpp#L83) 里两个都出现了：

```cpp
const auto c = campaign(advertiser_id, campaign_id);   // c 的类型是 std::optional<Campaign>
if (!c) return std::unexpected(StoreError::NotFound);  // 没找到 → 返回一个 "失败" 的 expected
...
if (c->status != CampaignStatus::Enabled) { ... }      // c-> 访问 optional 里的 Campaign
...
return res;                                             // 返回 ChargeResult → 自动变成 "成功" 的 expected
```

### 1.3 函数名前后的修饰词

| 写法 | 意思 | Java 对应 | 例子 |
|---|---|---|---|
| `virtual ... = 0;` | **纯虚函数**：只有签名，没有实现，子类必须实现 | `interface` 里的抽象方法 | `stores.h` 里所有方法 |
| `override` | "我重写了父类的虚函数"，签名写错了编译器会报错 | `@Override` | `now() const override` |
| `final`（写在类名后） | 这个类不能再被继承 | `final class` | `class SystemClock final : public Clock` |
| `const`（写在参数列表**后面**） | 这个函数**不修改对象自己的成员** | 没有对应 | `std::int64_t calls() const` |
| `virtual ~X() = default;` | **虚析构函数**。凡是要被继承、要通过父类指针删除的类都必须有 | 没有（Java 有 GC） | `virtual ~CampaignStore() = default;` |
| `= delete` | 禁止这个函数，比如禁止拷贝 | 没有 | `Conn(const Conn&) = delete;` |
| `explicit`（构造函数前） | 禁止隐式类型转换 | 没有 | `explicit FixedClock(time_point t)` |
| `[[noreturn]]` | 这个函数永远不会正常返回（一定会抛异常） | 没有 | `dynamo_store.cpp` 里的 `fail(...)` |
| `extern "C"` | 按 C 语言的规则导出函数 | 没有 | `extern "C" void on_signal(int)` |

**`const` 写在不同位置，意思完全不同**，这是初学时最容易混淆的地方：

```cpp
std::optional<Campaign> get(const std::string& id);   // 参数前的 const: 不修改这个参数
std::int64_t calls() const;                           // 参数列表后的 const: 不修改对象自己
const auto c = campaign(...);                         // 变量前的 const: c 赋值后不能再改
```

`const` 成员函数里想加锁怎么办？锁本身是会变的。答案是给它加 `mutable`，见 [budget_service.h:53](include/ads/budget_service.h#L53)：`mutable std::mutex m_;`，意思是"这个成员可以在 const 函数里修改"。

### 1.4 `.cpp` 里定义函数时的 `类名::`

`.h` 里在类内部声明：
```cpp
class BudgetService {
    Lease& lease_for(const std::string& campaign_id);
};
```
`.cpp` 里定义时，函数名前面要加 `BudgetService::`，告诉编译器"这是 BudgetService 的成员函数"。[budget_service.cpp:35](src/budget_service.cpp#L35)：
```cpp
BudgetService::Lease& BudgetService::lease_for(const std::string& campaign_id)
```
**返回类型前面也有一个 `BudgetService::`**，因为 `Lease` 是定义在 `BudgetService` 里面的嵌套类型。在类外面写它，必须带上类名前缀。

### 1.5 构造函数和初始化列表

[budget_service.cpp:21](src/budget_service.cpp#L21)：
```cpp
BudgetService::BudgetService(CampaignStore& campaigns, BudgetStore& budgets, LedgerStore& ledger,
                             const Clock& clock, BudgetConfig cfg)
    : campaigns_{campaigns}, budgets_{budgets}, ledger_{ledger}, clock_{clock}, cfg_{cfg}
{
}
```
- 冒号后面的部分叫**初始化列表**：在进入函数体 `{}` 之前，先把每个成员变量初始化好。
- **引用成员**（`CampaignStore& campaigns_;`）**只能**在这里初始化，因为引用一旦绑定就不能再改指向。
- 成员名后面加下划线（`campaigns_`）只是命名习惯，用来区分成员变量和参数。

⚠️ 保存引用意味着：**`BudgetService` 不拥有这些存储对象，调用方必须保证它们比 `BudgetService` 活得久。** 在 [main.cpp](src/main.cpp) 里，存储对象先于 `budget` 声明，按照 C++ 的规则，变量按声明的相反顺序销毁，所以 `budget` 会先被销毁，安全。

### 1.6 lambda：没有名字的函数

[api.cpp](src/api.cpp) 里每个路由都是一个 lambda：
```cpp
svr.Post("/v1/campaigns", [d](const httplib::Request& req, httplib::Response& res) {
    ...
});
```
| 部分 | 意思 |
|---|---|
| `[d]` | **捕获列表**：把外面的变量 `d` 拷贝一份带进 lambda 里 |
| `(const httplib::Request& req, httplib::Response& res)` | 参数：只读的请求、要填写的响应 |
| `{ ... }` | 函数体 |

几种捕获写法：

| 写法 | 意思 | 例子 |
|---|---|---|
| `[]` | 什么都不捕获 | `/health` 路由 |
| `[d]` | 按值拷贝 `d` | 所有业务路由。`d` 里面只有几个引用，拷贝很便宜 |
| `[&]` | 按引用捕获用到的所有外部变量 | main.cpp 的 watcher 线程（要访问 `svr`） |
| `[&](const auto& kv)` | 参数类型写 `auto`：**泛型 lambda**，参数是什么类型都行 | `invalidate` 里的 `erase_if` |

**为什么路由用 `[d]` 而不用 `[&]`？** lambda 被保存在 `svr` 里，会在 `register_routes` 函数返回很久之后才被调用。如果用 `[&]` 捕获参数 `d`，等调用时 `d` 早就销毁了，引用会悬空。

---

## 2. 代码里出现的其它语法

| 语法 | 意思 | 例子 |
|---|---|---|
| `namespace ads { ... }` | 命名空间，防止重名（≈ Java 的 package） | 所有代码都在 `ads` 里 |
| `namespace { ... }` | **匿名命名空间**：里面的东西只在当前 `.cpp` 里可见（≈ Java 的 `private static`） | `api.cpp` 里的 `reply`、`error` 辅助函数 |
| `using Micros = std::int64_t;` | 类型别名 | 金额统一叫 `Micros`，看到就知道是钱 |
| `constexpr Micros kMicrosPerDollar = 1'000'000;` | 编译期常量；数字里的 `'` 只是分隔符，方便阅读 | [model.h](include/ads/model.h) |
| `enum class Pacing { Standard, Accelerated };` | 强类型枚举，必须写 `Pacing::Standard` | 不会和别的枚举混用 |
| `Micros daily_budget{0};` | 成员的默认值 | `Campaign` 的字段 |
| `struct` vs `class` | 唯一区别：`struct` 默认 public，`class` 默认 private | 纯数据用 `struct`，有逻辑和不变式的用 `class` |
| `auto x = ...;` | 让编译器推断类型（≈ Java 的 `var`） | 到处都是 |
| `auto [it, inserted] = map.try_emplace(...);` | **结构化绑定**：把返回的 pair 拆成两个变量 | [budget_service.cpp:42](src/budget_service.cpp#L42) |
| `if (auto it = m.find(k); it != m.end())` | **带初始化的 if**：`it` 只在 if 里可见 | [budget_service.cpp:39](src/budget_service.cpp#L39) |
| `for (const auto& [k, c] : campaigns_)` | 遍历 map，同时拆出 key 和 value | `memory_store.cpp` 的 `list` |
| `std::lock_guard lock{m_};` | **RAII 加锁**：构造时加锁，离开 `{}` 时自动解锁，就算中途抛异常也会解锁（≈ `synchronized`） | 到处都是 |
| `std::shared_lock` / `std::unique_lock` | 读写锁的读锁 / 写锁（≈ `ReentrantReadWriteLock`） | `lease_for`：先用读锁查，查不到再换写锁 |
| `std::unique_ptr<T>` / `std::make_unique<T>()` | **独占所有权的智能指针**：它销毁时自动释放对象（不需要 GC） | `leases_` 里存的租约 |
| `R"sql( ... )sql"` | **原始字符串**：里面的引号、换行都不需要转义 | pg_ledger.cpp 里的 SQL |
| `std::format("{:016x}", n)` | 格式化字符串（≈ `String.format`） | `new_id()` |
| `static_cast<double>(x)` | 显式类型转换 | 金额计算 |
| `try { } catch (...) { throw; }` | `catch (...)` 捕获任何异常；`throw;` 原样重新抛出 | `charge` 里记账失败时退款 |
| `thread_local` | 每个线程各有一份 | `new_id()` 里的随机数生成器 |
| `#ifdef ADS_WITH_AWS` | 条件编译：只有打开这个开关时才编译这段代码 | `main.cpp` |
| `#pragma once` | 保证头文件只被包含一次 | 每个 `.h` 的第一行 |

---

## 3. 逐个文件讲

### 3.1 [model.h](include/ads/model.h)：数据结构

全部是 `struct` 和 `enum`，没有复杂逻辑：
- `Campaign`：一个广告活动。注意 `version` 字段（乐观锁用）。
- `ChargeEvent`：一次点击扣费，`event_id` 用来去重。
- `CampaignPatch`：PATCH 请求里每个字段都是 `std::optional`，**没传就是空，表示不修改**。这比 Java 里用 `null` 表示"没传"更明确。
- **金额用 `std::int64_t`（micros）而不用 `double`**：`0.1 + 0.2` 在浮点数里不等于 `0.3`，钱是不能有误差的。

### 3.2 [stores.h](include/ads/stores.h)：三个接口

```cpp
class CampaignStore {
public:
    virtual ~CampaignStore() = default;                 // ① 虚析构函数
    virtual std::optional<Campaign> get(...) = 0;      // ② 纯虚函数
};
```
- 一个类里只有 `virtual ... = 0` 的函数，就相当于 Java 的 `interface`。
- ① 为什么必须有虚析构函数：`main.cpp` 通过 `CampaignStore*` 这样的父类指针使用 `DynamoStore` 对象。如果没有虚析构函数，通过父类指针删除子类对象时只会调用父类的析构函数，子类的资源就漏了（这是未定义行为）。
- `struct Unavailable : std::runtime_error { using std::runtime_error::runtime_error; };`：自定义一个异常类型；`using ...` 这一行的意思是"直接复用父类的构造函数"。

**为什么要先定义接口**：业务逻辑（`BudgetService`、`api.cpp`）只依赖接口，不关心背后是内存、DynamoDB 还是 PostgreSQL。好处是在 macOS 上不装 AWS SDK 也能编译和测试，测试里可以换成内存实现。这种做法叫**依赖倒置**，和 Java 里面向接口编程、配合 Spring 注入是同一个思路，只是这里是在 `main` 里手动注入的。

### 3.3 [memory_store.cpp](src/memory_store.cpp)：最简单的实现

```cpp
class MemoryStore final : public CampaignStore, public BudgetStore, public LedgerStore {
```
- **一个类同时实现三个接口**（C++ 支持多继承；Java 里是 `implements A, B, C`）。
- 每个函数的第一行都是 `std::lock_guard lock{m_};`：一把锁保护所有数据，保证多线程安全。
- 数据用 `std::map<std::pair<std::string, std::string>, Campaign>` 存，key 是 `(广告主, campaign_id)`，相当于联合主键。

### 3.4 [budget_service.cpp](src/budget_service.cpp)：核心逻辑

最重要的是 `charge` 函数（[第 80 行](src/budget_service.cpp#L80)），按顺序做这些事：

| 行号 | 做什么 | 用到的 C++ |
|---|---|---|
| 83–84 | 读 campaign；找不到就返回错误 | `optional`、`std::unexpected` |
| 86–91 | 先假设每个事件都成功；campaign 没启用就全部标成 `CampaignNotActive` | `vector::assign`、`std::ranges::fill` |
| 94–98 | 标出同一批里重复或不合法的事件 | `unordered_set<string_view>`；`insert(...).second` 为 false 说明已经存在 |
| 100–105 | 算今天的日期、每次预支多少、匀速投放的上限 | `static_cast<double>` |
| 107 | 拿到这个 campaign 的本地租约 | 返回引用 `Lease&` |
| 110–144 | **加锁**，逐个事件：检查匀速 → 本地余额不够就去存储层预支 → 本地预扣 | `std::lock_guard`；离开 `{}` 自动解锁 |
| 147–154 | **在锁外**写记账（I/O 很慢，不能拿着锁去做）；失败就把预扣全部退回，并重新抛出异常 | `try / catch (...) / throw;` |
| 155–168 | 记账结果里重复的事件，把钱退回本地租约 | |

两个设计要点：
- **为什么 `leases_` 里存 `std::unique_ptr<Lease>`，而不是直接存 `Lease`？** `Lease` 里面有 `std::mutex`，mutex 不能被移动或拷贝。用 `unique_ptr` 包一层，Lease 对象本身就固定在堆上的一个位置，map 怎么调整都不影响它，`lease_for` 返回的引用也就一直有效。（[第 43 行](src/budget_service.cpp#L43)的注释）
- **析构函数里的 `try { release_all(); } catch (...) {}`**：C++ 的析构函数不能把异常抛出去（否则程序会直接终止），所以在里面吞掉。

### 3.5 [api.cpp](src/api.cpp)：HTTP 路由

结构很固定，每个路由都是：
```cpp
svr.Get("/v1/campaigns/:id", [d](const httplib::Request& req, httplib::Response& res) {
    auto adv = advertiser(req, res);    // 1. 取身份；失败时函数内部已经写好 401
    if (!adv) return;                    //    没有身份就直接返回
    const auto c = d.campaigns.get(*adv, req.path_params.at("id"));   // 2. 调业务逻辑
    if (!c) return error(res, 404, ...); // 3. 根据结果写响应
    with_etag(res, *c);
});
```
- `req.path_params.at("id")` 取出 URL 里 `:id` 位置的值。
- `return error(...)`：`error` 返回 `void`，`return` 一个 void 表达式在 C++ 里是合法的，意思是"做完这件事就返回"，可以少写一行。
- 文件顶部的 `namespace { ... }` 里是一些小工具函数，只在这个文件内使用。

### 3.6 [main.cpp](src/main.cpp)：组装一切

`main` 做的事情就是：创建存储对象 → 创建 `BudgetService` → 注册路由 → 开始监听 → 收到信号后清理。值得注意的几点：

1. **用指针在两种存储之间切换**（第 46–70 行）：
   ```cpp
   ads::CampaignStore* campaigns = nullptr;   // 接口指针
   if (memory) campaigns = mem.get();          // 指向 MemoryStore
   else        campaigns = dynamo.get();       // 或指向 DynamoStore
   ```
   `mem.get()` 从 `unique_ptr` 里取出裸指针。所有权仍然在 `mem` 手里，`campaigns` 只是"借来看看"。
2. **声明顺序就是生命周期**：变量按声明的**相反顺序**销毁。`aws_api` 最先声明，所以最后销毁，保证 AWS SDK 在所有 client 都销毁之后才关闭。
3. **信号处理**（第 22–23 行、88–93 行）：信号处理函数里能安全做的事情很少，所以 `on_signal` 只把一个 `std::atomic<bool>` 设成 true，再由一个普通线程（`watcher`）看到后调用 `svr.stop()`。
4. `std::jthread`：C++20 的线程类，析构时自动 `join`，而且可以接收一个 `std::stop_token` 来请求它停止（≈ Java 线程的 `interrupt`）。

### 3.7 [dynamo_store.cpp](src/dynamo_store.cpp)：DynamoDB 实现

- **pimpl 写法**（pointer to implementation）：头文件里只写 `struct Impl; std::unique_ptr<Impl> impl_;`，把 AWS SDK 的类型全部藏在 `.cpp` 里。好处是包含 `dynamo_store.h` 的文件不需要 AWS SDK 的头文件，编译快，以后换 SDK 版本也只需要重新编译这一个文件。
  - 代价是析构函数必须在 `.cpp` 里定义：`DynamoStore::~DynamoStore() = default;`，因为 `unique_ptr` 销毁 `Impl` 时需要知道它的完整定义，而这个定义只在 `.cpp` 里有。
- **`AwsApiGuard`**：RAII 的典型用法，构造时调用 `Aws::InitAPI`，析构时调用 `Aws::ShutdownAPI`，保证两者一定成对出现。
- `S("...")`、`N(123)` 这些小工具函数把 C++ 的值包装成 DynamoDB 的属性值，让后面的代码读起来更短。

### 3.8 [pg_ledger.cpp](src/pg_ledger.cpp)：PostgreSQL 实现

- **`unique_ptr` 加自定义删除函数**，用来管理 C 库的资源：
  ```cpp
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  Result r{PQexecParams(...), &PQclear};   // r 销毁时自动调用 PQclear(r)
  ```
  libpq 是 C 库，要求用完查询结果后手动调用 `PQclear`。用 `unique_ptr` 包一层，就不会忘记释放，抛异常时也会释放。
- **连接池里的 `Conn` 类**：构造时从池里借一个连接，析构时自动还回去。`Conn(const Conn&) = delete;` 禁止拷贝，否则一个连接会被归还两次。
- `$1, $2` 是**参数化查询**：SQL 和参数分开传给数据库，避免 SQL 注入（≈ JDBC 的 `PreparedStatement`）。

---

## 4. Java 对照速查

| C++（本项目里的写法） | Java 里差不多的东西 |
|---|---|
| `class X { virtual void f() = 0; };` | `interface X { void f(); }` |
| `class A final : public X` | `final class A implements X` |
| `override` | `@Override` |
| `std::optional<T>` | `Optional<T>` |
| `std::expected<T, E>` | 没有内置的，类似 Vavr 的 `Either`，或者 Rust 的 `Result` |
| `const T&` 参数 | 普通对象参数（但 Java 不能阻止你修改它） |
| `std::unique_ptr<T>` | 普通对象引用（但这里由作用域决定什么时候释放，而不是 GC） |
| `std::lock_guard lock{m};` | `synchronized (m) { ... }` |
| `std::shared_mutex` | `ReentrantReadWriteLock` |
| `std::atomic<bool>` | `AtomicBoolean` |
| `std::jthread` | `Thread`，加上自动 join |
| `[d](...) { ... }` lambda | `(req, res) -> { ... }`（Java lambda 只能捕获 effectively final 的变量，C++ 要你自己选是拷贝还是引用） |
| `namespace { }` 里的函数 | `private static` 方法 |
| 析构函数 `~X()` | 没有对应（`try-with-resources` 的 `close()` 最接近） |

---

## 5. 自测

看完可以试着回答下面几个问题，答案都在上面：

1. `get(const std::string& advertiser_id)` 如果改成 `get(std::string advertiser_id)`，程序结果会变吗？性能会变吗？
2. `lease_for` 返回 `Lease&`。如果 `leases_` 存的是 `Lease` 而不是 `unique_ptr<Lease>`，会有什么问题？
3. 为什么路由的 lambda 用 `[d]` 捕获，而 main.cpp 里的 watcher 线程可以用 `[&]`？
4. `std::int64_t calls() const` 里的 `const` 是什么意思？如果去掉，会影响谁？
5. 如果删掉 `virtual ~CampaignStore() = default;`，什么情况下会出问题？
