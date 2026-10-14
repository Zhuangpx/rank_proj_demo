// 演示程序：a 个用户同时对同一个物品反复购买和回退，每做完一次操作就查一次排行榜，
// 三类事件按发生顺序写进一个日志文件。它不判定通过与否，只记录实际发生了什么。
//
// 日志里的三类事件：
//   操作  谁在什么时候做了什么，引擎返回什么，真实数据变成多少
//   查询  这一刻榜单上是什么，和真实数据差多少
//   更新  榜单换版本了，把换版本前后的真实数据和榜单并排列出来
//
// 真实数据指的是 demo 自己数的净次数（买成功 +qty、退成功 -qty），
// 它等于引擎订单表里的求和，是「业务上到底卖了多少」；
// 榜单是引擎对外发布的快照，更新有延迟，两者的差就是延迟的大小。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rank/engine.h"

namespace fs = std::filesystem;

using rank::Config;
using rank::Consistency;
using rank::Engine;
using rank::ItemCount;
using rank::MetricsView;
using rank::Result;
using rank::TopKResult;

namespace {

using Clock = std::chrono::steady_clock;

struct Opts {
    uint64_t    users = 8;       // 同时操作的用户数，就是说明里的 a
    uint64_t    ops = 40;        // 每个用户做多少次操作
    uint64_t    item_id = 42;    // 所有用户都盯着这一个物品
    int64_t     qty = 1;         // 每次买卖的件数
    uint64_t    refund_pct = 35; // 回退占比
    uint64_t    think_ms = 10;   // 两次操作之间歇多久
    uint64_t    tmax_ms = 50;    // 榜单最多多久更新一次
    uint64_t    seed = 1;
    bool        fsync = false;   // 演示默认不等真落盘，开了会慢很多
    std::string dir = "demo_data";
    std::string log_path;        // 空的话就是 <dir>/demo.log
};

// 全程的计数，最后汇总用。
struct Stats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> ok_purchase{0};
    std::atomic<uint64_t> ok_refund{0};
    std::atomic<uint64_t> dup_purchase{0};
    std::atomic<uint64_t> dup_refund{0};
    std::atomic<uint64_t> not_found{0};
    std::atomic<uint64_t> other{0};
    std::atomic<int64_t>  true_net{0};  // 真实净次数：买成功 +qty，退成功 -qty
    std::atomic<uint64_t> queries{0};
    std::atomic<uint64_t> updates{0};
};

// printf 的 %-Ns 是按字节补空格的，一个中文字三个字节，直接用会对不齐。
// 这里自己数显示宽度：ASCII 算一格，其余一律算两格。
std::string PadDisp(const std::string& s, int width) {
    int w = 0;
    for (unsigned char c : s) {
        if (c < 0x80) ++w;
        else if ((c & 0xC0) != 0x80) w += 2;  // 多字节字符的头字节
    }
    std::string out = s;
    for (int i = w; i < width; ++i) out += ' ';
    return out;
}

std::string BoardStr(const std::vector<ItemCount>& v) {
    if (v.empty()) return "（空榜）";
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 0) s += "  ";
        s += "物品" + std::to_string(v[i].item_id) + "=" + std::to_string(v[i].count);
    }
    return s;
}

// 真实数据减榜单。一般是正的（榜单更新有延迟），偶尔会是负的：
// demo 是在 Purchase 返回之后才加自己那个计数的，而引擎在返回之前就已经把这笔算进去了，
// 所以有那么一小会儿榜单比 demo 自己数的还多。
std::string LagStr(int64_t diff) {
    if (diff >= 0) return "落后 " + std::to_string(diff);
    return "超前 " + std::to_string(-diff);
}

int64_t BoardCount(const std::vector<ItemCount>& v, uint64_t item_id) {
    for (const auto& x : v) {
        if (x.item_id == item_id) return x.count;
    }
    return 0;
}

// 日志。所有写入都在一把锁里，所以文件里的行就是事件真实发生的先后。
// 榜单换版本也是在这里发现的：每次查询都比一下版本号，变了就多记一条更新事件。
class EventLog {
public:
    EventLog(const std::string& path, uint64_t item_id) : item_id_(item_id) {
        f_ = std::fopen(path.c_str(), "wb");
        t0_ = Clock::now();
    }
    ~EventLog() {
        if (f_ != nullptr) std::fclose(f_);
    }
    bool Ok() const { return f_ != nullptr; }

    void Header(const Opts& o) {
        std::lock_guard<std::mutex> g(mu_);
        std::fprintf(f_, "# %llu 个用户同时对物品 %llu 反复购买和回退，每次操作之后查一次排行榜。\n",
                     (unsigned long long)o.users, (unsigned long long)o.item_id);
        std::fprintf(f_, "# 每人 %llu 次操作，其中约 %llu%% 是回退，每次 %lld 件，两次操作之间歇 %llu ms。\n",
                     (unsigned long long)o.ops, (unsigned long long)o.refund_pct, (long long)o.qty,
                     (unsigned long long)o.think_ms);
        std::fprintf(f_, "# 榜单最多 %llu ms 更新一次；写文件%s等真落盘。\n",
                     (unsigned long long)o.tmax_ms, o.fsync ? "" : "不");
        std::fprintf(f_, "# 行首是从开跑算起的毫秒数。三类事件：\n");
        std::fprintf(f_, "#   操作  谁做了什么，引擎返回什么，真实净次数变成多少\n");
        std::fprintf(f_, "#   查询  这一刻榜单上是什么，比真实数据落后多少\n");
        std::fprintf(f_, "#   更新  榜单换版本了，列出换版本前后的真实数据和榜单\n");
        std::fprintf(f_, "# 真实净次数是 demo 自己数的（买成功 +%lld，退成功 -%lld），\n",
                     (long long)o.qty, (long long)o.qty);
        std::fprintf(f_, "# 榜单是引擎发布的快照，两者的差就是更新延迟的大小。\n");
        std::fprintf(f_, "# 偶尔会出现「榜单超前」：demo 是在调用返回之后才加自己那个计数的，\n");
        std::fprintf(f_, "# 而引擎在返回之前就已经把这笔算进去了，所以有一小会儿榜单上的数更多。\n");
        std::fprintf(f_, "#%s\n", std::string(100, '-').c_str());
        std::fflush(f_);
    }

    void Op(uint64_t user, bool is_purchase, uint64_t order_id, int64_t qty, Result r,
            int64_t true_net_after) {
        std::lock_guard<std::mutex> g(mu_);
        std::fprintf(f_, "[%10.3f] 操作  用户%-3llu %s 订单=%-10llu ", Ms(),
                     (unsigned long long)user, is_purchase ? "购买" : "回退",
                     (unsigned long long)order_id);
        if (is_purchase) {
            std::fprintf(f_, "%+lld 件", (long long)qty);  // 显示宽度 5 格
        } else {
            std::fprintf(f_, "     ");
        }
        std::fprintf(f_, "  -> %-18s 真实净次数=%lld\n", rank::ToString(r), (long long)true_net_after);
    }

    // 查询结果进来，顺带比一下版本号。版本变了说明榜单在这次查询之前的某个时刻
    // 换了一版，就紧跟着补一条更新事件，把上一次查询看到的和这次看到的并排列出来。
    void Query(const TopKResult& res, const MetricsView& m, int64_t true_net, bool* out_updated) {
        std::lock_guard<std::mutex> g(mu_);
        int64_t on_board = BoardCount(res.entries, item_id_);
        std::fprintf(f_, "[%10.3f] 查询  版本=%-4llu 榜单: %s 真实净次数=%-8lld 榜单%s\n", Ms(),
                     (unsigned long long)res.version, PadDisp(BoardStr(res.entries), 24).c_str(),
                     (long long)true_net, LagStr(true_net - on_board).c_str());

        // 只认版本号变大的。查快照是在锁外面做的，两个线程可能先后拿到 v4 和 v5，
        // 却按相反的顺序进来写日志，看上去就成了 5 -> 4。引擎的版本号只增不减，
        // 所以这种倒着来的直接忽略，不然会多记出一次根本没发生的更新。
        bool changed = seen_ && res.version > prev_version_;
        if (changed) {
            int64_t prev_on_board = BoardCount(prev_board_, item_id_);
            std::fprintf(f_, "[%10.3f] 更新  榜单版本 %llu -> %llu（上面这次查询发现的）\n", Ms(),
                         (unsigned long long)prev_version_, (unsigned long long)res.version);
            std::fprintf(f_, "                   更新前  真实净次数=%-8lld 榜单: %s %s\n",
                         (long long)prev_true_, PadDisp(BoardStr(prev_board_), 24).c_str(),
                         LagStr(prev_true_ - prev_on_board).c_str());
            std::fprintf(f_, "                   更新后  真实净次数=%-8lld 榜单: %s %s\n",
                         (long long)true_net, PadDisp(BoardStr(res.entries), 24).c_str(),
                         LagStr(true_net - on_board).c_str());
            // 这一段统计的是「上一次换版本到这一次换版本」整个周期，
            // 不是上面那两行相邻两次查询之间。
            std::fprintf(f_,
                         "                   这一版之内落盘 %llu 笔操作，只往榜单上改了 %llu 个条目；"
                         "ts_merged %llu -> %llu\n",
                         (unsigned long long)(m.ops_durable - mark_m_.ops_durable),
                         (unsigned long long)(m.events_applied - mark_m_.events_applied),
                         (unsigned long long)mark_m_.last_ts_merged,
                         (unsigned long long)m.last_ts_merged);
            mark_m_ = m;
        }
        if (!seen_) {
            mark_m_ = m;
            seen_ = true;
        }
        // 每次查询都更新，这样下次发现换版本时，「更新前」拿到的是紧挨着的那一次查询。
        // 版本倒着来的那次不覆盖，留着已经见过的最新一版。
        if (res.version >= prev_version_) {
            prev_version_ = res.version;
            prev_board_ = res.entries;
            prev_true_ = true_net;
        }
        if (out_updated != nullptr) *out_updated = changed;
    }

    void Blank() {
        std::lock_guard<std::mutex> g(mu_);
        std::fprintf(f_, "\n");
    }

    // 汇总直接写文件，和屏幕上打的是同一份内容。
    void Summary(const std::string& text) {
        std::lock_guard<std::mutex> g(mu_);
        std::fprintf(f_, "%s", text.c_str());
        std::fflush(f_);
    }

private:
    double Ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0_).count();
    }

    std::FILE*             f_ = nullptr;
    std::mutex             mu_;
    Clock::time_point      t0_;
    uint64_t               item_id_;
    bool                   seen_ = false;
    uint64_t               prev_version_ = 0;   // 上一次查询看到的，每次查询都刷
    std::vector<ItemCount> prev_board_;
    int64_t                prev_true_ = 0;
    MetricsView            mark_m_{};           // 上一次换版本时的指标，只在换版本时刷
};

void UserThread(uint64_t user, Engine* e, EventLog* log, const Opts& o, Stats* st) {
    std::mt19937_64 rng(o.seed * 1000003ULL + user);
    // 每个用户用自己的一段订单号，互相不撞。
    const uint64_t base = 1000000ULL * (user + 1);
    std::vector<uint64_t> mine;  // 这个用户买过的订单号，回退时从里面挑
    mine.reserve(o.ops);

    for (uint64_t i = 0; i < o.ops; ++i) {
        bool want_refund = !mine.empty() && (rng() % 100) < o.refund_pct;
        Result r;
        uint64_t order_id;
        // 这次操作做完之后真实净次数是多少。成功的那两条直接取原子加返回的旧值再算，
        // 不能加完再 load——中间别的线程也在加，load 到的是别人的结果，
        // 打出来会出现「买了一笔反而变小」。
        int64_t net_after = 0;
        bool    net_known = false;
        if (want_refund) {
            // 十次里有一次故意退一个从来没买过的订单号，看引擎怎么回。
            if (rng() % 10 == 0) {
                order_id = base + 900000ULL + (rng() % 1000);
            } else {
                order_id = mine[rng() % mine.size()];
            }
            r = e->Refund(order_id);
            if (r == Result::Ok) {
                st->ok_refund.fetch_add(1);
                net_after = st->true_net.fetch_sub(o.qty) - o.qty;
                net_known = true;
            } else if (r == Result::AlreadyRefunded) {
                st->dup_refund.fetch_add(1);
            } else if (r == Result::OrderNotFound) {
                st->not_found.fetch_add(1);
            } else {
                st->other.fetch_add(1);
            }
        } else {
            // 五次里有一次重复提交上一笔，模拟调用方超时重试。
            if (!mine.empty() && rng() % 5 == 0) {
                order_id = mine.back();
            } else {
                order_id = base + i;
            }
            r = e->Purchase(order_id, user, o.item_id, o.qty);
            if (r == Result::Ok) {
                st->ok_purchase.fetch_add(1);
                net_after = st->true_net.fetch_add(o.qty) + o.qty;
                net_known = true;
                mine.push_back(order_id);
            } else if (r == Result::AlreadyPurchased) {
                st->dup_purchase.fetch_add(1);
            } else {
                st->other.fetch_add(1);
            }
        }
        st->calls.fetch_add(1);
        // 被挡下的那几种不改真实净次数，打当时的总数就行。
        if (!net_known) net_after = st->true_net.load();
        log->Op(user, !want_refund, order_id, o.qty, r, net_after);

        // 每次操作之后查一次榜。Stale 是直接读已发布的快照，不逼引擎更新，
        // 这样榜单落后多少才看得出来。
        TopKResult res = e->TopK(10, Consistency::Stale);
        MetricsView m = e->GetMetrics();
        bool updated = false;
        log->Query(res, m, st->true_net.load(), &updated);
        st->queries.fetch_add(1);
        if (updated) st->updates.fetch_add(1);

        if (o.think_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(o.think_ms));
        }
    }
}

void Usage() {
    std::printf(
        "用法: rank_demo [选项]\n"
        "  --users <a>        同时操作的用户数，默认 8\n"
        "  --ops <n>          每个用户做多少次操作，默认 40\n"
        "  --item <id>        所有用户都操作这一个物品，默认 42\n"
        "  --qty <n>          每次买卖几件，默认 1\n"
        "  --refund-pct <p>   回退占比，默认 35\n"
        "  --think-ms <n>     两次操作之间歇多久，默认 10（给榜单更新留出间隔）\n"
        "  --tmax <ms>        榜单最多多久更新一次，默认 50\n"
        "  --seed <n>         随机种子，默认 1\n"
        "  --fsync            每批都等真落盘，默认不等\n"
        "  --dir <目录>       数据目录，默认 demo_data，跑之前会清空\n"
        "  --log <文件>       日志文件，默认 <数据目录>/demo.log\n");
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next_u = [&]() -> uint64_t { return std::strtoull(argv[++i], nullptr, 10); };
        if (a == "--users" && i + 1 < argc) o.users = next_u();
        else if (a == "--ops" && i + 1 < argc) o.ops = next_u();
        else if (a == "--item" && i + 1 < argc) o.item_id = next_u();
        else if (a == "--qty" && i + 1 < argc) o.qty = static_cast<int64_t>(next_u());
        else if (a == "--refund-pct" && i + 1 < argc) o.refund_pct = next_u();
        else if (a == "--think-ms" && i + 1 < argc) o.think_ms = next_u();
        else if (a == "--tmax" && i + 1 < argc) o.tmax_ms = next_u();
        else if (a == "--seed" && i + 1 < argc) o.seed = next_u();
        else if (a == "--fsync") o.fsync = true;
        else if (a == "--dir" && i + 1 < argc) o.dir = argv[++i];
        else if (a == "--log" && i + 1 < argc) o.log_path = argv[++i];
        else if (a == "--help" || a == "-h") {
            Usage();
            return 0;
        } else {
            Usage();
            return 2;
        }
    }
    if (o.users == 0 || o.ops == 0 || o.qty <= 0) {
        std::fprintf(stderr, "用户数、操作次数、件数都得大于 0\n");
        return 2;
    }
    if (o.log_path.empty()) o.log_path = o.dir + "/demo.log";

    std::error_code ec;
    fs::remove_all(o.dir, ec);
    fs::create_directories(o.dir, ec);

    Config cfg;
    cfg.data_dir = o.dir;
    cfg.t_max_ms = static_cast<uint32_t>(o.tmax_ms);
    cfg.enable_fsync = o.fsync;
    cfg.expect_orders = 1 << 16;
    auto e = Engine::Open(cfg);
    if (!e) {
        std::fprintf(stderr, "引擎起不来，目录 %s\n", o.dir.c_str());
        return 3;
    }

    EventLog log(o.log_path, o.item_id);
    if (!log.Ok()) {
        std::fprintf(stderr, "日志文件写不了：%s\n", o.log_path.c_str());
        return 3;
    }
    log.Header(o);

    std::printf("%llu 个用户同时对物品 %llu 反复买和退，每人 %llu 次操作，每次操作后查一次榜。\n",
                (unsigned long long)o.users, (unsigned long long)o.item_id,
                (unsigned long long)o.ops);
    std::printf("日志写到 %s\n", o.log_path.c_str());

    Stats st;
    // 先记一条基线，让日志开头就有「还没人操作时榜单长什么样」。
    {
        TopKResult res = e->TopK(10, Consistency::Stale);
        MetricsView m = e->GetMetrics();
        log.Query(res, m, 0, nullptr);
        log.Blank();
    }

    Clock::time_point start = Clock::now();
    std::vector<std::thread> ths;
    ths.reserve(o.users);
    for (uint64_t u = 0; u < o.users; ++u) {
        ths.emplace_back(UserThread, u, e.get(), &log, o, &st);
    }
    for (auto& t : ths) t.join();
    double wall_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

    log.Blank();
    // 最后强制更新一次，把还没进榜的增量全部合进去，再和真实数据对一遍。
    log.Summary("# 下面这次查询是所有用户都做完之后补的，走 Fresh，\n"
                "# 它会逼引擎立刻把剩下的增量合进榜单，所以榜单在这之后不再落后。\n");
    TopKResult final_res = e->TopK(10, Consistency::Fresh);
    MetricsView m = e->GetMetrics();
    log.Query(final_res, m, st.true_net.load(), nullptr);

    int64_t board = BoardCount(final_res.entries, o.item_id);
    int64_t order_table = 0;
    for (const auto& x : e->OrderTableSum()) {
        if (x.item_id == o.item_id) order_table = x.count;
    }
    int64_t truth = st.true_net.load();

    char buf[4096];
    int n = std::snprintf(
        buf, sizeof(buf),
        "\n== 汇总 ==\n"
        "用户数 %llu，每人 %llu 次操作，一共调了 %llu 次，耗时 %.1f ms\n"
        "  购买成功 %llu，回退成功 %llu\n"
        "  被判重挡下：重复购买 %llu，重复回退 %llu；订单不存在 %llu；其他 %llu\n"
        "排行榜查询 %llu 次，其中 %llu 次发现榜单换了版本\n"
        "已落盘操作 %llu 笔 -> 应用到榜单 %llu 个条目，合并比 %.4f\n"
        "最后对一遍账（三个数来自三条不同的路）：\n"
        "  demo 自己数的真实净次数     物品%llu = %lld\n"
        "  引擎订单表求和              物品%llu = %lld\n"
        "  强制更新之后的榜单          物品%llu = %lld\n"
        "  -> %s\n",
        (unsigned long long)o.users, (unsigned long long)o.ops,
        (unsigned long long)st.calls.load(), wall_ms, (unsigned long long)st.ok_purchase.load(),
        (unsigned long long)st.ok_refund.load(), (unsigned long long)st.dup_purchase.load(),
        (unsigned long long)st.dup_refund.load(), (unsigned long long)st.not_found.load(),
        (unsigned long long)st.other.load(), (unsigned long long)st.queries.load(),
        (unsigned long long)st.updates.load(), (unsigned long long)m.ops_durable,
        (unsigned long long)m.events_applied, m.CoalescingRatio(),
        (unsigned long long)o.item_id, (long long)truth, (unsigned long long)o.item_id,
        (long long)order_table, (unsigned long long)o.item_id, (long long)board,
        (truth == order_table && order_table == board) ? "三个数一致" : "对不上！");
    if (n > 0) {
        std::printf("%s", buf);
        log.Summary(buf);
    }
    return (truth == order_table && order_table == board) ? 0 : 1;
}
