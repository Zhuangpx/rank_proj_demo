// 模块 C：刷新与延迟（C-01 ~ C-06、C-09、C-10）
// C-07、C-08 是「半自动」用例，用压测入口跑出数据人看，不在这里。
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "test_util.h"

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

// C-01：没有任何读请求，定时也会把榜单刷出来。
TEST(C_Flush, C01_TimerFlushWithoutAnyRead) {
    rt::TempDir  dir("c01");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 50;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 10, 3), Result::Ok);
    rt::SleepMs(200);
    auto r = e->TopK(10, Consistency::Stale);
    EXPECT_EQ(rt::CountOf(r.entries, 10), int64_t(3));
}

// C-02：停止写入超过 T_max 之后，Stale 读的结果就等于真值（INV-4）。
TEST(C_Flush, C02_StaleConvergesAfterWritesStop) {
    rt::TempDir  dir("c02");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 100;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> written{0};
    std::thread writer([&] {
        for (uint64_t i = 1; !stop.load(std::memory_order_acquire); ++i) {
            if (e->Purchase(i, i, i % 200 + 1, 1) == Result::Ok) written.fetch_add(1);
        }
    });
    rt::SleepMs(1000);
    stop.store(true, std::memory_order_release);
    writer.join();
    ASSERT_GT(written.load(), uint64_t(0));

    rt::SleepMs(c.t_max_ms * 3);  // 等过 T_max，期间没有任何写入和 Fresh 读

    auto from_engine = rt::ToMap(e->DumpAll(Consistency::Stale).entries);
    auto from_file = ops_scan::NonZero(ops_scan::ScanOpsLog(dir.file("ops.log")).counts);
    EXPECT_EQ(from_engine, from_file);
}

// C-03：Fresh 读受 T_min 约束，刷新次数远小于读次数。
// 必须用多个读线程：单线程串行读的话，每次读都是「等满 T_min、刷一次、返回」，
// 读次数天然等于刷新次数，测不出任何东西。T_min 真正省掉的是同一窗口内
// 多个读请求各刷一次。
TEST(C_Flush, C03_FreshReadsRateLimitedByTMin) {
    rt::TempDir  dir("c03");
    rank::Config c = rt::TestConfig(dir);
    c.t_min_ms = 50;
    c.t_max_ms = 60000;  // 把定时刷新排除掉，只留 Fresh 读这一个触发源
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 10, 1), Result::Ok);
    uint64_t before = e->GetMetrics().flush_count;

    const int             kReaders = 8;
    const uint64_t        kRunMs = 2000;
    std::atomic<uint64_t> reads{0};
    auto                  deadline = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(kRunMs);
    std::vector<std::thread> ts;
    for (int i = 0; i < kReaders; ++i) {
        ts.emplace_back([&] {
            while (std::chrono::steady_clock::now() < deadline) {
                e->TopK(10, Consistency::Fresh);
                reads.fetch_add(1);
            }
        });
    }
    for (auto& t : ts) t.join();
    uint64_t flushes = e->GetMetrics().flush_count - before;

    EXPECT_GT(reads.load(), uint64_t(0));
    // 2 s / 50 ms = 40 次是理论上限。Windows 上定时等待会被系统时钟节拍放大，
    // 实际只会更少，这里卡上界留一倍余量。
    EXPECT_LE(flushes, uint64_t(2 * kRunMs / c.t_min_ms))
        << "读了 " << reads.load() << " 次，刷新了 " << flushes << " 次";
    EXPECT_LT(flushes * 2, reads.load()) << "刷新次数和读次数接近，说明最小间隔没生效";
}

// C-04：同时到达的多个 Fresh 读共享同一次刷新（FR-10）。
TEST(C_Flush, C04_ConcurrentFreshReadsCoalesce) {
    rt::TempDir  dir("c04");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 60000;
    c.t_min_ms = 50;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 10, 1), Result::Ok);
    e->TopK(10, Consistency::Fresh);  // 定基线，顺便让 last_flush 刚刚发生
    rt::SleepMs(c.t_min_ms * 2);
    uint64_t before = e->GetMetrics().flush_count;

    std::atomic<bool>        go{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 32; ++i) {
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            e->TopK(10, Consistency::Fresh);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();

    uint64_t delta = e->GetMetrics().flush_count - before;
    EXPECT_GE(delta, uint64_t(1));
    EXPECT_LE(delta, uint64_t(8)) << "32 个同时到达的 Fresh 读触发了 " << delta << " 次刷新";
}

// C-05：定时和读都没触发时，待刷新量攒够了自己会刷（FR-9 第三类触发）。
TEST(C_Flush, C05_PendingCountTriggersFlush) {
    rt::TempDir  dir("c05");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 60000;
    c.t_min_ms = 1;
    c.n_pending_max = 100;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 500; ++i) ASSERT_EQ(e->Purchase(i, i, i, 1), Result::Ok);

    // 不发 Fresh 读，只用 Stale 轮询，看榜单会不会自己更新
    bool updated = false;
    for (int i = 0; i < 100 && !updated; ++i) {
        if (e->TopK(10, Consistency::Stale).entries.size() > 0) updated = true;
        else rt::SleepMs(20);
    }
    EXPECT_TRUE(updated) << "攒够 N_pending_max 也没刷新";
    EXPECT_GE(e->GetMetrics().flush_count, uint64_t(1));
}

// C-06：陈旧度 = 当前时间 - ts_merged，不应该明显超过 T_max。
TEST(C_Flush, C06_StalenessBounded) {
    rt::TempDir  dir("c06");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 100;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (uint64_t i = 1; !stop.load(std::memory_order_acquire); ++i) {
            e->Purchase(i, i, i % 100 + 1, 1);
        }
    });

    // 余量给够：Windows 上条件变量的定时等待会被系统时钟节拍（约 15.6 ms）向上取整，
    // 再加上一次刷新本身的耗时和线程调度抖动。
    const uint64_t limit_ns = (c.t_max_ms + 300) * 1000000ULL;
    uint64_t       worst = 0;
    for (int i = 0; i < 50; ++i) {
        rt::SleepMs(10);
        uint64_t ts = e->TopK(10, Consistency::Stale).ts_merged;
        uint64_t now = rt::NowNs();
        uint64_t st = now > ts ? now - ts : 0;
        if (st > worst) worst = st;
    }
    stop.store(true, std::memory_order_release);
    writer.join();

    EXPECT_LE(worst, limit_ns) << "最差陈旧度 " << worst / 1000000 << " ms，T_max = " << c.t_max_ms
                               << " ms";
}

// C-09：一笔操作都没有的情况下，Fresh 读必须能正常返回，不能卡死。
// 这条是日志线程「空队列时也要处理切表请求」那段逻辑的回归用例。
TEST(C_Flush, C09_FreshReadOnIdleEngine) {
    rt::TempDir dir("c09");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    auto t0 = std::chrono::steady_clock::now();
    auto r = e->TopK(10, Consistency::Fresh);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();

    EXPECT_EQ(r.code, Result::Ok);
    EXPECT_EQ(r.entries.size(), size_t(0));
    EXPECT_LT(ms, 1000) << "空引擎上的 Fresh 读花了 " << ms << " ms";
    EXPECT_GT(r.ts_merged, uint64_t(0));
}

// C-10：跑一轮之后五项指标都要有值且合理（FR-13）。
TEST(C_Flush, C10_MetricsAreSane) {
    rt::TempDir dir("c10");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 2000; ++i) ASSERT_EQ(e->Purchase(i, i, i % 100 + 1, 2), Result::Ok);
    for (uint64_t i = 1; i <= 500; ++i) ASSERT_EQ(e->Refund(i), Result::Ok);
    e->TopK(10, Consistency::Fresh);

    rank::MetricsView m = e->GetMetrics();
    EXPECT_EQ(m.ops_durable, uint64_t(2500));
    EXPECT_GE(m.flush_count, uint64_t(1));
    EXPECT_GT(m.flush_ns_total, uint64_t(0));
    EXPECT_GE(m.flush_ns_max, uint64_t(0));
    EXPECT_GT(m.events_applied, uint64_t(0));
    EXPECT_GT(m.CoalescingRatio(), 0.0);
    EXPECT_LE(m.CoalescingRatio(), 1.0);
    EXPECT_GT(m.last_ts_merged, uint64_t(0));
    EXPECT_LE(m.staleness_ns, 5ULL * 1000000000ULL);
    // 提交进去多少 delta，就必须有多少被算进榜单（INV-9）
    EXPECT_EQ(m.delta_submitted, m.delta_applied);
    EXPECT_EQ(m.delta_submitted, int64_t(2000 * 2 - 500 * 2));
}

}  // namespace
