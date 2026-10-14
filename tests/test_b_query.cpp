// 模块 B：排行榜查询（B-01 ~ B-12）
#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "test_util.h"

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

// B-01：按计数从大到小。
TEST(B_Query, B01_SortedByCountDesc) {
    rt::TempDir dir("b01");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 1, 5), Result::Ok);
    ASSERT_EQ(e->Purchase(2, 1, 2, 9), Result::Ok);
    ASSERT_EQ(e->Purchase(3, 1, 3, 7), Result::Ok);

    auto r = e->TopK(3, Consistency::Fresh);
    ASSERT_EQ(r.entries.size(), size_t(3));
    EXPECT_EQ(r.entries[0].item_id, uint64_t(2));
    EXPECT_EQ(r.entries[0].count, int64_t(9));
    EXPECT_EQ(r.entries[1].item_id, uint64_t(3));
    EXPECT_EQ(r.entries[1].count, int64_t(7));
    EXPECT_EQ(r.entries[2].item_id, uint64_t(1));
    EXPECT_EQ(r.entries[2].count, int64_t(5));
}

// B-02：计数相同按 item_id 从小到大（INV-7，排名必须唯一确定）。
TEST(B_Query, B02_TieBreakByItemId) {
    rt::TempDir dir("b02");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 7, 4), Result::Ok);
    ASSERT_EQ(e->Purchase(2, 1, 3, 4), Result::Ok);
    ASSERT_EQ(e->Purchase(3, 1, 9, 4), Result::Ok);

    auto r = e->TopK(3, Consistency::Fresh);
    ASSERT_EQ(r.entries.size(), size_t(3));
    EXPECT_EQ(r.entries[0].item_id, uint64_t(3));
    EXPECT_EQ(r.entries[1].item_id, uint64_t(7));
    EXPECT_EQ(r.entries[2].item_id, uint64_t(9));
}

// B-03：计数归零不进榜。
TEST(B_Query, B03_ZeroCountNotListed) {
    rt::TempDir dir("b03");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 5, 3), Result::Ok);
    ASSERT_EQ(e->Refund(1), Result::Ok);
    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(r.entries.size(), size_t(0));
}

// B-04：k 超过上限直接拒，不给半截结果。
TEST(B_Query, B04_KOverMaxRejected) {
    rt::TempDir dir("b04");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 5, 3), Result::Ok);
    auto r = e->TopK(1001, Consistency::Stale);
    EXPECT_EQ(r.code, Result::InvalidArg);
    EXPECT_EQ(r.entries.size(), size_t(0));
}

// B-05：k 大于实际物品数，返回有多少给多少。
TEST(B_Query, B05_KLargerThanAvailable) {
    rt::TempDir dir("b05");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 3; ++i) ASSERT_EQ(e->Purchase(i, i, i, 1), Result::Ok);
    auto r = e->TopK(100, Consistency::Fresh);
    EXPECT_EQ(r.code, Result::Ok);
    EXPECT_EQ(r.entries.size(), size_t(3));
}

// B-06：k = 0 返回空列表，不算错。
TEST(B_Query, B06_KZero) {
    rt::TempDir dir("b06");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 1, 5, 3), Result::Ok);
    e->TopK(10, Consistency::Fresh);
    auto r = e->TopK(0, Consistency::Stale);
    EXPECT_EQ(r.code, Result::Ok);
    EXPECT_EQ(r.entries.size(), size_t(0));
}

// B-07：Stale 读不触发刷新。把 T_max 调到 60 s 排除定时刷新的干扰。
TEST(B_Query, B07_StaleReadDoesNotTriggerFlush) {
    rt::TempDir  dir("b07");
    rank::Config c = rt::TestConfig(dir);
    c.t_max_ms = 60000;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 20; ++i) ASSERT_EQ(e->Purchase(i, i, i % 5 + 1, 1), Result::Ok);
    e->TopK(10, Consistency::Fresh);  // 先刷一次，把基线定住

    uint64_t before = e->GetMetrics().flush_count;
    for (int i = 0; i < 1000; ++i) {
        auto r = e->TopK(10, Consistency::Stale);
        EXPECT_EQ(r.code, Result::Ok);
    }
    uint64_t after = e->GetMetrics().flush_count;
    EXPECT_EQ(before, after) << "1000 次 Stale 读触发了 " << (after - before) << " 次刷新";
}

// B-08：拿到 OK 之后紧接着的 Fresh 读一定看得到这一笔（INV-5）。重复 200 轮。
TEST(B_Query, B08_FreshReadSeesJustAckedWrite) {
    rt::TempDir dir("b08");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 200; ++i) {
        ASSERT_EQ(e->Purchase(i, i, 42, 1), Result::Ok);
        auto r = e->TopK(10, Consistency::Fresh);
        ASSERT_EQ(rt::CountOf(r.entries, 42), static_cast<int64_t>(i)) << "第 " << i << " 轮";
    }
}

// B-09：一次调用返回的内容来自同一个版本，不会半新半旧（INV-8）。
// 做法是把每个版本号第一次见到的内容记下来，之后再见到同一个版本号必须一模一样。
TEST(B_Query, B09_SnapshotIsConsistent) {
    rt::TempDir dir("b09");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (uint64_t i = 1; !stop.load(std::memory_order_acquire); ++i) {
            e->Purchase(i, i, i % 50 + 1, 1);
        }
    });

    // 按时间读而不是按次数读：Stale 读是纯读快照指针，2000 次几微秒就跑完了，
    // 一次刷新（T_max = 50 ms）都还没发生，这条用例就什么也没验到。
    std::map<uint64_t, std::vector<rank::ItemCount>> seen;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    uint64_t loops = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++loops;
        auto r = e->TopK(100, Consistency::Stale);
        auto it = seen.find(r.version);
        if (it == seen.end()) {
            seen.emplace(r.version, r.entries);
        } else {
            ASSERT_EQ(it->second.size(), r.entries.size()) << "版本 " << r.version << " 内容变了";
            for (size_t j = 0; j < r.entries.size(); ++j) {
                ASSERT_EQ(it->second[j].item_id, r.entries[j].item_id);
                ASSERT_EQ(it->second[j].count, r.entries[j].count);
            }
        }
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    EXPECT_GT(loops, uint64_t(0));
    EXPECT_GT(seen.size(), size_t(1)) << "读了 " << loops
                                      << " 次只见到一个版本，这条用例没测到东西";
}

// B-10：版本号和 ts_merged 只增不减。
TEST(B_Query, B10_VersionAndTsMonotonic) {
    rt::TempDir dir("b10");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (uint64_t i = 1; !stop.load(std::memory_order_acquire); ++i) {
            e->Purchase(i, i, i % 20 + 1, 1);
        }
    });

    uint64_t last_v = 0, last_ts = 0;
    for (int i = 0; i < 3000; ++i) {
        auto r = e->TopK(10, Consistency::Stale);
        ASSERT_GE(r.version, last_v);
        ASSERT_GE(r.ts_merged, last_ts);
        last_v = r.version;
        last_ts = r.ts_merged;
    }
    stop.store(true, std::memory_order_release);
    writer.join();
}

// B-11：ts_merged 会随刷新往前走，但不会跑到当前时间之后。
TEST(B_Query, B11_TsMergedAdvancesAndNeverExceedsNow) {
    rt::TempDir dir("b11");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    uint64_t t1 = e->TopK(10, Consistency::Stale).ts_merged;
    EXPECT_LE(t1, rt::NowNs());

    for (uint64_t i = 1; i <= 50; ++i) ASSERT_EQ(e->Purchase(i, i, 1, 1), Result::Ok);
    rt::SleepMs(200);  // 远超 T_max（50 ms），定时刷新必定已经发生

    uint64_t t2 = e->TopK(10, Consistency::Stale).ts_merged;
    EXPECT_GT(t2, t1);
    EXPECT_LE(t2, rt::NowNs());
}

// B-12：Fresh 读返回的榜单，是这次调用之后才合出来的。
TEST(B_Query, B12_FreshReadTsMergedAfterCallStart) {
    rt::TempDir dir("b12");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    for (int round = 0; round < 20; ++round) {
        uint64_t t = rt::NowNs();
        auto     r = e->TopK(10, Consistency::Fresh);
        EXPECT_EQ(r.code, Result::Ok);
        EXPECT_GT(r.ts_merged, t) << "第 " << round << " 轮：返回的是本次调用之前的旧榜单";
    }
}

}  // namespace
