#include "ads/pg_ledger.h"

#include <libpq-fe.h>

#include <condition_variable>
#include <mutex>
#include <unordered_set>

namespace ads {

namespace {

using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

// 参数化查询 ($1, $2 ...)：值和 SQL 分开传，不会有 SQL 注入
Result exec(PGconn* c, const char* sql, std::initializer_list<std::string> params)
{
    std::vector<const char*> vals;
    for (const auto& p : params) vals.push_back(p.c_str());
    Result r{PQexecParams(c, sql, static_cast<int>(vals.size()), nullptr, vals.data(), nullptr, nullptr, 0), &PQclear};
    const auto st = PQresultStatus(r.get());
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        const std::string msg = PQerrorMessage(c);
        if (PQstatus(c) != CONNECTION_OK) throw Unavailable("postgres: " + msg);   // 断线 → 503，可以重试
        throw std::runtime_error("postgres: " + msg);                               // SQL 错误 → 程序 bug
    }
    return r;
}

}  // namespace

struct PgLedger::Pool {
    std::string conninfo;
    std::mutex m;
    std::condition_variable cv;
    std::vector<PGconn*> idle;

    // RAII 借连接: 析构时自动还回池里 (异常路径也不会漏还)
    class Conn {
    public:
        Conn(Pool& p, PGconn* c) : p_{p}, c_{c} {}
        ~Conn()
        {
            {
                std::lock_guard lock{p_.m};
                p_.idle.push_back(c_);
            }
            p_.cv.notify_one();
        }
        Conn(const Conn&) = delete;
        Conn& operator=(const Conn&) = delete;
        PGconn* get() const { return c_; }

    private:
        Pool& p_;
        PGconn* c_;
    };

    Conn acquire()
    {
        std::unique_lock lock{m};
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return !idle.empty(); }))
            throw Unavailable("postgres: connection pool exhausted");
        PGconn* c = idle.back();
        idle.pop_back();
        lock.unlock();
        if (PQstatus(c) != CONNECTION_OK) PQreset(c);   // 数据库重启 / 主备切换后自动重连
        return Conn{*this, c};
    }

    ~Pool()
    {
        for (auto* c : idle) PQfinish(c);
    }
};

PgLedger::PgLedger(const std::string& conninfo, int pool_size) : pool_{std::make_unique<Pool>()}
{
    pool_->conninfo = conninfo;
    for (int i = 0; i < pool_size; ++i) {
        PGconn* c = PQconnectdb(conninfo.c_str());
        if (PQstatus(c) != CONNECTION_OK) {
            const std::string msg = PQerrorMessage(c);
            PQfinish(c);
            throw std::runtime_error("postgres connect failed: " + msg);
        }
        pool_->idle.push_back(c);
    }
}

PgLedger::~PgLedger() = default;

void PgLedger::migrate()
{
    auto conn = pool_->acquire();
    exec(conn.get(), "SELECT pg_advisory_lock(727001)", {});
    exec(conn.get(), R"sql(
        CREATE TABLE IF NOT EXISTS spend_ledger (
            event_id      TEXT        NOT NULL,
            advertiser_id TEXT        NOT NULL,
            campaign_id   TEXT        NOT NULL,
            spend_date    DATE        NOT NULL,
            cost_micros   BIGINT      NOT NULL CHECK (cost_micros > 0),
            recorded_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
            PRIMARY KEY (campaign_id, event_id)
        ))sql", {});
    // 迁移: 旧版本的表主键只有 event_id → 改成 (campaign_id, event_id)。重复执行无副作用 (幂等迁移)
    exec(conn.get(), R"sql(
        DO $$
        BEGIN
            IF (SELECT COUNT(*) FROM information_schema.key_column_usage
                 WHERE table_name = 'spend_ledger' AND constraint_name = 'spend_ledger_pkey') = 1 THEN
                ALTER TABLE spend_ledger DROP CONSTRAINT spend_ledger_pkey,
                                         ADD PRIMARY KEY (campaign_id, event_id);
            END IF;
        END $$)sql", {});
    exec(conn.get(), "CREATE INDEX IF NOT EXISTS spend_ledger_adv_date ON spend_ledger (advertiser_id, spend_date)", {});
    exec(conn.get(), "CREATE INDEX IF NOT EXISTS spend_ledger_cmp_date ON spend_ledger (campaign_id, spend_date)", {});
    exec(conn.get(), "SELECT pg_advisory_unlock(727001)", {});
}

std::vector<bool> PgLedger::record(const std::string& advertiser_id, const std::string& campaign_id, const std::string& date,
                                   std::span<const ChargeEvent> events)
{
    // 一条 SQL 批量插入: 把 event_id 和金额作为两个数组传进去，unnest 展开成行
    // (id 已经按 [A-Za-z0-9_-] 白名单校验过，可以直接拼成数组字面量 {a,b,c})
    std::string ids = "{", costs = "{";
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) {
            ids += ',';
            costs += ',';
        }
        ids += events[i].event_id;
        costs += std::to_string(events[i].cost);
    }
    ids += '}';
    costs += '}';

    auto conn = pool_->acquire();
    auto r = exec(conn.get(), R"sql(
        INSERT INTO spend_ledger (event_id, advertiser_id, campaign_id, spend_date, cost_micros)
        SELECT e, $2, $3, $4::date, c FROM unnest($1::text[], $5::bigint[]) AS t(e, c)
        ON CONFLICT (campaign_id, event_id) DO NOTHING
        RETURNING event_id)sql",
                  {ids, advertiser_id, campaign_id, date, costs});
    std::unordered_set<std::string> inserted;
    for (int i = 0; i < PQntuples(r.get()); ++i) inserted.insert(PQgetvalue(r.get(), i, 0));
    std::vector<bool> out;
    out.reserve(events.size());
    for (const auto& e : events) out.push_back(inserted.contains(e.event_id));
    return out;
}

Micros PgLedger::spent(const std::string& campaign_id, const std::string& date)
{
    auto conn = pool_->acquire();
    auto r = exec(conn.get(),
                  "SELECT COALESCE(SUM(cost_micros), 0)::bigint FROM spend_ledger WHERE campaign_id = $1 AND spend_date = $2::date",
                  {campaign_id, date});
    return std::stoll(PQgetvalue(r.get(), 0, 0));
}

std::vector<DailySpendRow> PgLedger::report(const std::string& advertiser_id, const std::string& from, const std::string& to)
{
    auto conn = pool_->acquire();
    auto r = exec(conn.get(), R"sql(
        WITH daily AS (
            SELECT spend_date, campaign_id, COUNT(*) AS events, SUM(cost_micros) AS spend
              FROM spend_ledger
             WHERE advertiser_id = $1 AND spend_date BETWEEN $2::date AND $3::date
             GROUP BY spend_date, campaign_id
        )
        SELECT spend_date::text, campaign_id, events, spend,
               spend::float8 / SUM(spend) OVER (PARTITION BY spend_date)          AS share_of_advertiser,
               SUM(spend) OVER (PARTITION BY campaign_id ORDER BY spend_date)     AS running_total
          FROM daily
         ORDER BY spend_date, spend DESC)sql",
                  {advertiser_id, from, to});
    std::vector<DailySpendRow> out;
    for (int i = 0; i < PQntuples(r.get()); ++i)
        out.push_back({PQgetvalue(r.get(), i, 0), PQgetvalue(r.get(), i, 1), std::stoll(PQgetvalue(r.get(), i, 2)),
                       std::stoll(PQgetvalue(r.get(), i, 3)), std::stod(PQgetvalue(r.get(), i, 4)),
                       std::stoll(PQgetvalue(r.get(), i, 5))});
    return out;
}

}  // namespace ads
