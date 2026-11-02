// SQLite 的 RAII 封装: 连接、prepared statement、事务
// Java 对应: JDBC 的 Connection / PreparedStatement / try-with-resources
// 生产里换成 Postgres (libpqxx) 思路完全一样: prepared statement + 显式事务 + 批量提交
#pragma once
#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace common {

class SqlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Stmt {
public:
    Stmt(sqlite3* db, std::string_view sql)
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr) != SQLITE_OK)
            throw SqlError(std::string("prepare failed: ") + sqlite3_errmsg(db) + " | " + std::string(sql));
        stmt_.reset(raw);
    }

    // 链式绑定，下标从 1 开始 (和 JDBC 一样)
    Stmt& bind(int i, std::int64_t v) { check(sqlite3_bind_int64(stmt_.get(), i, v)); return *this; }
    Stmt& bind(int i, int v) { return bind(i, static_cast<std::int64_t>(v)); }
    Stmt& bind(int i, double v) { check(sqlite3_bind_double(stmt_.get(), i, v)); return *this; }
    Stmt& bind(int i, std::string_view v)
    {
        // SQLITE_TRANSIENT: 让 SQLite 拷贝一份，调用方的 string 可以马上释放
        check(sqlite3_bind_text(stmt_.get(), i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
        return *this;
    }

    // 返回 true 表示还有一行结果
    bool step()
    {
        const int rc = sqlite3_step(stmt_.get());
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw SqlError(std::string("step failed: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_.get())));
    }

    // 执行一次并重置，方便在循环里复用同一个 prepared statement (批量插入的关键)
    void exec_and_reset()
    {
        step();
        reset();
    }

    void reset()
    {
        sqlite3_reset(stmt_.get());
        sqlite3_clear_bindings(stmt_.get());
    }

    std::int64_t col_int(int i) const { return sqlite3_column_int64(stmt_.get(), i); }
    double col_double(int i) const { return sqlite3_column_double(stmt_.get(), i); }
    std::string col_text(int i) const
    {
        auto p = sqlite3_column_text(stmt_.get(), i);
        return p ? reinterpret_cast<const char*>(p) : "";
    }

private:
    void check(int rc)
    {
        if (rc != SQLITE_OK) throw SqlError(std::string("bind failed: ") + sqlite3_errstr(rc));
    }
    struct Finalizer {
        void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
    };
    std::unique_ptr<sqlite3_stmt, Finalizer> stmt_;   // 自定义 deleter 的 unique_ptr = 最常用的 RAII 写法
};

class Db {
public:
    explicit Db(const std::string& path)
    {
        sqlite3* raw = nullptr;
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
            std::string msg = raw ? sqlite3_errmsg(raw) : "out of memory";
            sqlite3_close(raw);
            throw SqlError("open failed: " + msg);
        }
        db_.reset(raw);
        sqlite3_busy_timeout(raw, 10'000);    // 多个连接同时写时等锁，而不是立刻报 SQLITE_BUSY
        exec("PRAGMA journal_mode=WAL");      // 写不阻塞读
        exec("PRAGMA synchronous=NORMAL");    // WAL 下的常用折中: 断电可能丢最后几个事务，但不会损坏
        exec("PRAGMA foreign_keys=ON");
    }

    void exec(std::string_view sql)
    {
        char* err = nullptr;
        if (sqlite3_exec(db_.get(), std::string(sql).c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw SqlError("exec failed: " + msg + " | " + std::string(sql));
        }
    }

    Stmt prepare(std::string_view sql) { return Stmt{db_.get(), sql}; }

    // 单值查询的便捷函数
    std::int64_t scalar_int(std::string_view sql)
    {
        auto s = prepare(sql);
        return s.step() ? s.col_int(0) : 0;
    }

    std::int64_t changes() const { return sqlite3_changes64(db_.get()); }
    sqlite3* raw() const { return db_.get(); }

private:
    struct Closer {
        void operator()(sqlite3* d) const { sqlite3_close(d); }
    };
    std::unique_ptr<sqlite3, Closer> db_;
};

// 事务守卫: 构造时 BEGIN，commit() 提交；没 commit 就析构 (比如中途抛异常) → 自动 ROLLBACK
class Transaction {
public:
    explicit Transaction(Db& db) : db_{db} { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!done_) {
            try { db_.exec("ROLLBACK"); } catch (...) {}   // 析构函数不能抛异常
        }
    }
    void commit()
    {
        db_.exec("COMMIT");
        done_ = true;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

private:
    Db& db_;
    bool done_{false};
};

}  // namespace common
