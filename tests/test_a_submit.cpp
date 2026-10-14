// 模块 A：操作提交与幂等（A-01 ~ A-13）
#include <atomic>
#include <thread>
#include <vector>

#include "test_util.h"

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

// A-01：最简单的一笔买，Fresh 读立刻能看到。
TEST(A_Submit, A01_PurchaseThenFreshRead) {
    rt::TempDir dir("a01");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    EXPECT_EQ(e->Purchase(1, 100, 10, 1), Result::Ok);
    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(r.code, Result::Ok);
    ASSERT_EQ(r.entries.size(), size_t(1));
    EXPECT_EQ(r.entries[0].item_id, uint64_t(10));
    EXPECT_EQ(r.entries[0].count, int64_t(1));
}

// A-02：qty 是数量不是次数，一笔 qty=5 就是 +5。
TEST(A_Submit, A02_QuantityIsAmountNotCount) {
    rt::TempDir dir("a02");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    EXPECT_EQ(e->Purchase(1, 100, 10, 5), Result::Ok);
    auto r = e->TopK(10, Consistency::Fresh);
    ASSERT_EQ(r.entries.size(), size_t(1));
    EXPECT_EQ(r.entries[0].count, int64_t(5));
    // 只写了一条记录，不是 5 条
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(32));
}

// A-03：同一个订单号重复提交只生效一次。
TEST(A_Submit, A03_RepeatPurchaseIsIdempotent) {
    rt::TempDir dir("a03");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    EXPECT_EQ(e->Purchase(1, 100, 10, 3), Result::Ok);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(e->Purchase(1, 100, 10, 3), Result::AlreadyPurchased) << "第 " << i << " 次重试";
    }
    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r.entries, 10), int64_t(3));
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(32));
}

// A-04 / A-05 / A-07 是一条链：退掉之后计数归零、重复退无效、订单号不能复用。
TEST(A_Submit, A04_A05_A07_RefundChain) {
    rt::TempDir dir("a04");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 100, 10, 3), Result::Ok);
    for (int i = 0; i < 4; ++i) e->Purchase(1, 100, 10, 3);

    // A-04：退掉之后计数归零，归零的物品不进榜
    EXPECT_EQ(e->Refund(1), Result::Ok);
    auto r1 = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r1.entries, 10), int64_t(0));
    EXPECT_EQ(r1.entries.size(), size_t(0)) << "计数为 0 的物品不应该出现在榜上";

    // A-05：重复退只认第一次
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(e->Refund(1), Result::AlreadyRefunded) << "第 " << i << " 次重退";
    }
    auto r2 = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r2.entries, 10), int64_t(0));

    // A-07：退掉的订单号不能再拿来买
    EXPECT_EQ(e->Purchase(1, 100, 10, 3), Result::AlreadyPurchased);
    auto r3 = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r3.entries, 10), int64_t(0));

    // 全程只该写两条记录：一条 +3，一条 -3
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(64));
}

// A-06：退一个不存在的订单，不写任何记录。
TEST(A_Submit, A06_RefundUnknownOrder) {
    rt::TempDir dir("a06");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    EXPECT_EQ(e->Refund(999), Result::OrderNotFound);
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(0));
}

// A-08：参数不合法直接拒掉，连文件都不碰。
TEST(A_Submit, A08_InvalidArgs) {
    rt::TempDir dir("a08");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    EXPECT_EQ(e->Purchase(1, 100, 10, 0), Result::InvalidArg);
    EXPECT_EQ(e->Purchase(2, 100, 10, -1), Result::InvalidArg);
    EXPECT_EQ(e->Purchase(3, 100, 0, 1), Result::InvalidArg);
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(0));
}

// A-09：按字节检查记录格式——定长 32、delta 带符号、ts 严格递增。
TEST(A_Submit, A09_RecordLayout) {
    rt::TempDir dir("a09");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 100, 10, 2), Result::Ok);
    ASSERT_EQ(e->Refund(1), Result::Ok);

    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(64));
    auto recs = ops_scan::ScanOpsLogForOrder(dir.file("ops.log"), 1);
    ASSERT_EQ(recs.size(), size_t(2));
    EXPECT_EQ(recs[0].item_id, uint64_t(10));
    EXPECT_EQ(recs[0].delta, int64_t(2));
    EXPECT_EQ(recs[1].item_id, uint64_t(10)) << "回退记录必须自带 item_id";
    EXPECT_EQ(recs[1].delta, int64_t(-2));
    EXPECT_GT(recs[1].ts, recs[0].ts);

    auto ops = ops_scan::ScanOpsLog(dir.file("ops.log"));
    EXPECT_EQ(ops.tail_bytes, uint64_t(0));
    EXPECT_TRUE(ops.ts_strictly_increasing);
    EXPECT_EQ(ops.purchases, uint64_t(1));
    EXPECT_EQ(ops.refunds, uint64_t(1));
}

// A-10：64 个线程抢同一个订单号，只能有一个赢。
TEST(A_Submit, A10_ConcurrentSameOrderOnlyOneWins) {
    rt::TempDir dir("a10");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    const int            kThreads = 64;
    std::atomic<int>     ok{0}, dup{0}, other{0};
    std::atomic<bool>    go{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            Result r = e->Purchase(1, 100, 10, 2);
            if (r == Result::Ok) ok.fetch_add(1);
            else if (r == Result::AlreadyPurchased) dup.fetch_add(1);
            else other.fetch_add(1);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();

    EXPECT_EQ(ok.load(), 1);
    EXPECT_EQ(dup.load(), kThreads - 1);
    EXPECT_EQ(other.load(), 0);
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(32)) << "只该落一条记录";
    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r.entries, 10), int64_t(2));
}

// A-11：买和退同时抢同一个订单，结果只可能是 0 或 2，中间读到的数不能为负。
TEST(A_Submit, A11_ConcurrentPurchaseAndRefund) {
    rt::TempDir dir("a11");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 100, 10, 2), Result::Ok);

    std::atomic<bool>        go{false}, stop{false};
    std::atomic<int>         refund_ok{0}, purchase_ok{0};
    std::atomic<bool>        saw_negative{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 32; ++i) {
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (e->Purchase(1, 100, 10, 2) == Result::Ok) purchase_ok.fetch_add(1);
        });
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            if (e->Refund(1) == Result::Ok) refund_ok.fetch_add(1);
        });
    }
    // 读线程全程盯着，看有没有出现负数（INV-2）
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto r = e->TopK(10, Consistency::Stale);
            for (const auto& x : r.entries) {
                if (x.count < 0) saw_negative.store(true);
            }
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();
    stop.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(purchase_ok.load(), 0) << "订单号已占用，不该再有购买成功";
    EXPECT_LE(refund_ok.load(), 1) << "重复的回退只能生效一次";
    EXPECT_FALSE(saw_negative.load());

    auto r = e->TopK(10, Consistency::Fresh);
    int64_t c = rt::CountOf(r.entries, 10);
    EXPECT_TRUE(c == 0 || c == 2) << "最终计数只可能是 0 或 2，实际是 " << c;
    EXPECT_EQ(c, refund_ok.load() == 1 ? int64_t(0) : int64_t(2));
}

// A-12：1000 笔买、退掉 300 笔，净值要对。
TEST(A_Submit, A12_NetCount) {
    rt::TempDir dir("a12");
    auto        e = Engine::Open(rt::TestConfig(dir));
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 1000; ++i) ASSERT_EQ(e->Purchase(i, i, 10, 2), Result::Ok);
    for (uint64_t i = 1; i <= 300; ++i) ASSERT_EQ(e->Refund(i), Result::Ok);

    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r.entries, 10), int64_t(1400));
    rt::ExpectMatchesOpsLog(*e, dir);
    rt::ExpectMatchesOrderTable(*e);
}

// A-13：拿到 OK 的那一刻，记录必须已经在文件里（FR-5）。这条要开真 fsync。
TEST(A_Submit, A13_OkMeansDurable) {
    rt::TempDir  dir("a13");
    rank::Config c = rt::TestConfig(dir);
    c.enable_fsync = true;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    ASSERT_EQ(e->Purchase(1, 100, 10, 1), Result::Ok);
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(32)) << "返回 OK 时数据还没落盘";
}

}  // namespace
