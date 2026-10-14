// 模块 E：并发与压力（E-01、E-02、E-03、E-05）
// E-04（64 线程满压）和 E-06（持续 5 分钟）用压测入口跑，不放在用例里：
//   跑一次就是分钟级，塞进单元测试套件会让每次改代码都等上几分钟。
// E-07 的做法是 rank_tests --gtest_repeat=50，不需要单独写一条用例。
#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "test_util.h"

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

struct LoadStat {
    std::atomic<uint64_t> purchase_ok{0};
    std::atomic<uint64_t> refund_ok{0};
    std::atomic<int64_t>  delta_sum{0};
};

// 每个线程占一段互不相交的订单号（高位放线程号），退的也只退自己买过的。
// 这样「成功次数」就是确定的：没有线程间的订单号争抢，任何一次失败都说明实现有问题。
void MixedLoad(Engine& e, int threads, uint64_t per_thread, uint64_t items, int refund_pct,
               LoadStat* st) {
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            std::mt19937_64        rng(0x1234 + static_cast<uint64_t>(t));
            std::vector<uint64_t>  bought;
            uint64_t               base = (static_cast<uint64_t>(t) + 1) << 40;
            uint64_t               local_p = 0, local_r = 0;
            int64_t                local_d = 0;
            for (uint64_t i = 0; i < per_thread; ++i) {
                uint64_t oid = base + i;
                uint64_t iid = rng() % items + 1;
                if (e.Purchase(oid, t + 1, iid, 2) == Result::Ok) {
                    ++local_p;
                    local_d += 2;
                    bought.push_back(oid);
                }
                if (!bought.empty() && static_cast<int>(rng() % 100) < refund_pct) {
                    size_t   k = rng() % bought.size();
                    uint64_t victim = bought[k];
                    if (e.Refund(victim) == Result::Ok) {
                        ++local_r;
                        local_d -= 2;
                    }
                    bought[k] = bought.back();
                    bought.pop_back();
                }
            }
            st->purchase_ok.fetch_add(local_p);
            st->refund_ok.fetch_add(local_r);
            st->delta_sum.fetch_add(local_d);
        });
    }
    for (auto& th : ts) th.join();
}

// E-01：32 线程混合压测，结束后榜单、ops.log、订单表三者对账一致。
TEST(E_Concurrency, E01_MixedLoadReconciles) {
    rt::TempDir  dir("e01");
    rank::Config c = rt::TestConfig(dir);
    c.expect_orders = 1 << 18;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    LoadStat st;
    MixedLoad(*e, 32, 5000, 2000, 25, &st);

    // 订单号互不相交，所以每一笔购买都必须成功
    EXPECT_EQ(st.purchase_ok.load(), uint64_t(32 * 5000));
    EXPECT_GT(st.refund_ok.load(), uint64_t(0));
    // 每笔退货退的都是自己刚买成功的订单，所以退货也必须全部成功，
    // 落盘记录数 = 购买数 + 退货数
    EXPECT_EQ(dir.size_of("ops.log"), 32 * (st.purchase_ok.load() + st.refund_ok.load()));

    rt::ExpectMatchesOpsLog(*e, dir);
    rt::ExpectMatchesOrderTable(*e);

    int64_t total = 0;
    for (const auto& kv : rt::ToMap(e->DumpAll(Consistency::Fresh).entries)) total += kv.second;
    EXPECT_EQ(total, st.delta_sum.load());
}

// E-02：读写同时进行，读到的计数不能是负数，也不能超过已提交的总量。
TEST(E_Concurrency, E02_ReadsStayPlausibleUnderWrites) {
    rt::TempDir  dir("e02");
    rank::Config c = rt::TestConfig(dir);
    c.expect_orders = 1 << 17;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    std::atomic<bool>     stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<int64_t>  bad_negative{-1};
    std::atomic<int64_t>  bad_too_big{-1};
    std::atomic<bool>     bad_order{false};
    const int64_t         ceiling = 2 * 16 * 3000;  // 所有线程全部买同一个物品时的上限

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                auto r = e->TopK(100, Consistency::Stale);
                reads.fetch_add(1);
                int64_t prev = INT64_MAX;
                for (const auto& x : r.entries) {
                    if (x.count < 0) bad_negative.store(x.count);
                    if (x.count > ceiling) bad_too_big.store(x.count);
                    if (x.count > prev) bad_order.store(true);  // 必须是不增序列
                    prev = x.count;
                }
            }
        });
    }

    LoadStat st;
    MixedLoad(*e, 16, 3000, 50, 30, &st);
    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();

    EXPECT_GT(reads.load(), uint64_t(0));
    EXPECT_EQ(bad_negative.load(), int64_t(-1)) << "读到了负数计数（INV-2）";
    EXPECT_EQ(bad_too_big.load(), int64_t(-1)) << "读到的计数超过了提交总量";
    EXPECT_FALSE(bad_order.load()) << "返回的榜单不是按计数降序的";
    rt::ExpectMatchesOpsLog(*e, dir);
}

// E-03：提交进待刷新表的 delta 总和，等于已经算进榜单的 delta 总和（INV-9）。
TEST(E_Concurrency, E03_DeltaSubmittedEqualsApplied) {
    rt::TempDir  dir("e03");
    rank::Config c = rt::TestConfig(dir);
    c.expect_orders = 1 << 17;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    LoadStat st;
    MixedLoad(*e, 16, 4000, 500, 25, &st);

    // 写线程都退出了，这次 Fresh 读会把剩下的增量全部合进去
    e->DumpAll(Consistency::Fresh);
    rank::MetricsView m = e->GetMetrics();
    EXPECT_EQ(m.delta_submitted, m.delta_applied);
    EXPECT_EQ(m.delta_applied, st.delta_sum.load());
    EXPECT_EQ(m.ops_durable, st.purchase_ok.load() + st.refund_ok.load());
    // 合并确实在起作用：应用到榜单的条目数应该明显少于落盘操作数
    EXPECT_LT(m.events_applied, m.ops_durable) << "合并比 " << m.CoalescingRatio();
}

// E-05：把提交队列压到很小，提交线程会频繁等待队列让位——
// 要的是「等了但不卡死，而且一笔不丢」。
TEST(E_Concurrency, E05_SmallQueueCapBlocksButLosesNothing) {
    rt::TempDir  dir("e05");
    rank::Config c = rt::TestConfig(dir);
    c.queue_cap = 8;  // 32 个线程抢 8 个位置，几乎每次提交都要等
    c.batch_size = 4;
    c.expect_orders = 1 << 16;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    LoadStat st;
    MixedLoad(*e, 32, 500, 100, 25, &st);

    EXPECT_EQ(st.purchase_ok.load(), uint64_t(32 * 500));
    EXPECT_EQ(dir.size_of("ops.log"), 32 * (st.purchase_ok.load() + st.refund_ok.load()));
    rt::ExpectMatchesOpsLog(*e, dir);
    rt::ExpectMatchesOrderTable(*e);
}

}  // namespace
