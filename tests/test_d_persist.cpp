// 模块 D：持久化与恢复（D-01 ~ D-11）
// 崩溃类用例靠 rank_crash_helper：它是一个独立进程，跑到指定位置直接终止，
// 不做任何收尾。只有另起进程才能测出「进程被拔电」的行为。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "test_util.h"

namespace fs = std::filesystem;

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

// 跑一批固定的混合操作：n 笔购买，每第 3 笔退掉上一笔。
// 返回成功落盘的操作条数，用来核对 ops.log 的长度。
uint64_t RunMixedLoad(Engine& e, uint64_t n) {
    uint64_t acked = 0;
    for (uint64_t i = 1; i <= n; ++i) {
        if (e.Purchase(i, i, i % 500 + 1, 2) == Result::Ok) ++acked;
        if (i % 3 == 0 && e.Refund(i) == Result::Ok) ++acked;
    }
    return acked;
}

std::map<uint64_t, int64_t> OpsLogCounts(const rt::TempDir& dir) {
    return ops_scan::NonZero(ops_scan::ScanOpsLog(dir.file("ops.log")).counts);
}

// D-01 / D-02：正常退出再重启，榜单与 ops.log 一致，两个文件长度都对。
TEST(D_Persist, D01_D02_CleanRestart) {
    rt::TempDir  dir("d01");
    rank::Config c = rt::TestConfig(dir);
    c.expect_orders = 1 << 17;
    c.rank_dat_period_ms = 50;

    uint64_t acked = 0;
    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        acked = RunMixedLoad(*e, 100000);
        ASSERT_EQ(acked, uint64_t(100000 + 100000 / 3));
    }  // 析构 = 正常退出：刷最后一次、写 rank.dat

    // D-02：ops.log 长度 = 32 × 成功操作数
    EXPECT_EQ(dir.size_of("ops.log"), 32 * acked);
    // D-02：rank.dat 长度 = 16 + 16 × 条目数
    ops_scan::RankDatResult rd = ops_scan::ReadRankDat(dir.file("rank.dat"));
    ASSERT_TRUE(rd.exists);
    EXPECT_TRUE(rd.valid);
    EXPECT_EQ(rd.file_size, 16 + 16 * rd.entries);
    // 干净退出时最后一次刷新把所有记录都算进去了，所以 rank.dat 必须和整份
    // ops.log 对得上（FR-16）
    rt::ExpectRankDatMatchesOpsLog(dir);
    EXPECT_EQ(ops_scan::ScanOpsLog(dir.file("ops.log"), rd.ts_merged).records_after, uint64_t(0));

    auto e2 = Engine::Open(c);
    ASSERT_TRUE(e2 != nullptr);
    rt::ExpectMatchesOpsLog(*e2, dir);
    rt::ExpectMatchesOrderTable(*e2);
}

// D-03：rank.dat 丢了，从 ops.log 全量重算，结果一样。
TEST(D_Persist, D03_MissingRankDat) {
    rt::TempDir  dir("d03");
    rank::Config c = rt::TestConfig(dir);
    c.rank_dat_period_ms = 20;

    std::map<uint64_t, int64_t> expect;
    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        RunMixedLoad(*e, 20000);
        expect = rt::ToMap(e->DumpAll(Consistency::Fresh).entries);
    }
    ASSERT_TRUE(dir.exists("rank.dat"));
    fs::remove(dir.file("rank.dat"));

    auto e2 = Engine::Open(c);
    ASSERT_TRUE(e2 != nullptr);
    EXPECT_EQ(rt::ToMap(e2->DumpAll(Consistency::Fresh).entries), expect);
    EXPECT_EQ(rt::ToMap(e2->DumpAll(Consistency::Fresh).entries), OpsLogCounts(dir));
}

// D-04：rank.dat 长度对不上就整份作废，不能只用一半。
TEST(D_Persist, D04_TruncatedRankDat) {
    rt::TempDir  dir("d04");
    rank::Config c = rt::TestConfig(dir);
    c.rank_dat_period_ms = 20;

    std::map<uint64_t, int64_t> expect;
    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        RunMixedLoad(*e, 20000);
        expect = rt::ToMap(e->DumpAll(Consistency::Fresh).entries);
    }
    uint64_t before = dir.size_of("rank.dat");
    ASSERT_GT(before, uint64_t(16));
    fs::resize_file(dir.file("rank.dat"), before - 8);
    // 先确认这份文件确实已经不合法了，否则这条用例没测到东西
    ASSERT_FALSE(ops_scan::ReadRankDat(dir.file("rank.dat")).valid);

    auto e2 = Engine::Open(c);
    ASSERT_TRUE(e2 != nullptr);
    EXPECT_EQ(rt::ToMap(e2->DumpAll(Consistency::Fresh).entries), expect);
}

// D-05：ops.log 末尾多出半条记录，启动时丢弃并截断。
TEST(D_Persist, D05_TornTailInOpsLog) {
    rt::TempDir  dir("d05");
    rank::Config c = rt::TestConfig(dir);

    std::map<uint64_t, int64_t> expect;
    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        RunMixedLoad(*e, 10000);
        expect = rt::ToMap(e->DumpAll(Consistency::Fresh).entries);
    }
    uint64_t before = dir.size_of("ops.log");
    {
        std::ofstream f(dir.file("ops.log"), std::ios::binary | std::ios::app);
        std::mt19937  rng(12345);
        for (int i = 0; i < 20; ++i) f.put(static_cast<char>(rng() & 0xff));
    }
    ASSERT_EQ(dir.size_of("ops.log"), before + 20);
    ASSERT_NE(ops_scan::ScanOpsLog(dir.file("ops.log")).tail_bytes, uint64_t(0));

    auto e2 = Engine::Open(c);
    ASSERT_TRUE(e2 != nullptr);
    EXPECT_EQ(rt::ToMap(e2->DumpAll(Consistency::Fresh).entries), expect);
    EXPECT_EQ(dir.size_of("ops.log"), before) << "多出来的半条没有被截掉";
    EXPECT_EQ(ops_scan::ScanOpsLog(dir.file("ops.log")).tail_bytes, uint64_t(0));
}

// D-06：崩在「写 ops.log 之前」——这笔不算成功，订单号还能再用。
TEST(D_Persist, D06_CrashBeforeWrite) {
    rt::TempDir dir("d06");
    int rc = rt::RunCrashHelper("--dir \"" + dir.path() + "\" --crash before_write --ops 1 "
                                "--order-base 500 --items 4 --qty 3");
    EXPECT_EQ(rc, 42) << "崩溃注入没生效，进程是正常退出的";
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(0));

    rank::Config c = rt::TestConfig(dir);
    auto         e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);
    // 同一个 order_id 重试：上一次没落盘，所以这次必须算新的
    EXPECT_EQ(e->Purchase(500, 500, 7, 3), Result::Ok);
    auto r = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r.entries, 7), int64_t(3));
}

// D-07：崩在「fsync 之后、返回调用方之前」——已落盘的不能丢，重试不能翻倍。
TEST(D_Persist, D07_CrashAfterFsyncBeforeAck) {
    rt::TempDir dir("d07");
    int rc = rt::RunCrashHelper("--dir \"" + dir.path() + "\" --crash after_fsync --ops 1 "
                                "--order-base 500 --items 1 --qty 3");
    EXPECT_EQ(rc, 42);
    // 调用方没拿到 OK，但记录已经在文件里了
    ASSERT_EQ(dir.size_of("ops.log"), uint64_t(32));

    rank::Config c = rt::TestConfig(dir);
    auto         e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);
    auto r1 = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r1.entries, 1), int64_t(3)) << "已落盘的操作在重启后丢了";

    // 重试同一个 order_id：必须被幂等挡住
    EXPECT_EQ(e->Purchase(500, 500, 1, 3), Result::AlreadyPurchased);
    auto r2 = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r2.entries, 1), int64_t(3)) << "重试把计数翻倍了";
    EXPECT_EQ(dir.size_of("ops.log"), uint64_t(32));
}

// D-08：崩在刷新中途——取走的增量丢了，但恢复时按 ts 重算，不多不少。
TEST(D_Persist, D08_CrashDuringFlush) {
    rt::TempDir dir("d08");
    int rc = rt::RunCrashHelper("--dir \"" + dir.path() + "\" --crash during_flush --skip 5 "
                                "--ops 10000 --order-base 1 --items 200 --qty 2 "
                                "--refund-pct 20 --tmax 20 --rank-period 20");
    EXPECT_EQ(rc, 42);
    ASSERT_GT(dir.size_of("ops.log"), uint64_t(0));

    rank::Config c = rt::TestConfig(dir);
    auto         e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);
    rt::ExpectMatchesOpsLog(*e, dir);
    rt::ExpectMatchesOrderTable(*e);
}

// D-09：崩在「写完 .tmp 还没改名」——rank.dat 还是崩溃前那份完整文件。
TEST(D_Persist, D09_CrashAfterTmpBeforeRename) {
    rt::TempDir dir("d09");
    // 先正常跑一轮，让 rank.dat 有一份完整内容
    int rc = rt::RunCrashHelper("--dir \"" + dir.path() +
                                    "\" --ops 2000 --order-base 1 --items 50 --qty 2 "
                                    "--refund-pct 20 --tmax 20 --rank-period 20",
                                dir.file("helper_out.txt"));
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(dir.exists("rank.dat"));
    ops_scan::RankDatResult rd1 = ops_scan::ReadRankDat(dir.file("rank.dat"));
    ASSERT_TRUE(rd1.valid);

    // 再跑一轮，这次写完 .tmp 就崩
    rc = rt::RunCrashHelper("--dir \"" + dir.path() + "\" --crash before_rename --ops 2000 "
                            "--order-base 100000 --items 50 --qty 2 --tmax 20 --rank-period 20");
    EXPECT_EQ(rc, 42);

    ops_scan::RankDatResult rd2 = ops_scan::ReadRankDat(dir.file("rank.dat"));
    EXPECT_TRUE(rd2.valid) << "rank.dat 被写坏了，整文件替换没做到";
    EXPECT_EQ(rd2.ts_merged, rd1.ts_merged) << "rank.dat 不该被这次崩溃改动";
    EXPECT_EQ(rd2.counts, rd1.counts);
    // 这份 rank.dat 是上一轮留下的，所以第二轮写的记录都在 ts_merged 之后；
    // 按「ts <= ts_merged」这条线切开，它和 ops.log 仍然对得上
    rt::ExpectRankDatMatchesOpsLog(dir);
    EXPECT_GT(ops_scan::ScanOpsLog(dir.file("ops.log"), rd2.ts_merged).records_after, uint64_t(0));

    rank::Config c = rt::TestConfig(dir);
    auto         e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);
    rt::ExpectMatchesOpsLog(*e, dir);
}

// D-10：随机时刻杀进程，重启对账，循环多轮。
// 计划里这条写的是 20 轮；这里跑 20 轮，每轮用不同的存活时长命中不同的内部状态。
TEST(D_Persist, D10_RandomKillLoop) {
    for (int round = 0; round < 20; ++round) {
        rt::TempDir dir("d10_" + std::to_string(round));
        uint64_t    alive_ms = 15 + static_cast<uint64_t>(round) * 7;
        int         rc = rt::RunCrashHelper(
            "--dir \"" + dir.path() + "\" --ops 200000 --order-base 1 --items 300 --qty 2 "
            "--refund-pct 25 --tmax 20 --rank-period 30 --seed " + std::to_string(round + 1) +
            " --exit-after-ms " + std::to_string(alive_ms));
        // 操作数给得很大，正常跑不完就被杀；万一机器太快跑完了也不算失败
        EXPECT_TRUE(rc == 42 || rc == 0) << "第 " << round << " 轮退出码 " << rc;

        rank::Config c = rt::TestConfig(dir);
        auto         e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr) << "第 " << round << " 轮重启失败";

        auto from_engine = rt::ToMap(e->DumpAll(Consistency::Fresh).entries);
        auto from_file = OpsLogCounts(dir);
        EXPECT_EQ(from_engine, from_file) << "第 " << round << " 轮（存活 " << alive_ms
                                          << " ms）对账不一致";
        EXPECT_EQ(ops_scan::ScanOpsLog(dir.file("ops.log")).tail_bytes, uint64_t(0));
    }
}

// D-11：重启后继续写，新记录的 ts 接在历史后面，不会和旧记录冲突。
TEST(D_Persist, D11_TimestampsContinueAcrossRestart) {
    rt::TempDir  dir("d11");
    rank::Config c = rt::TestConfig(dir);

    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        RunMixedLoad(*e, 5000);
    }
    uint64_t last_ts_before = ops_scan::ScanOpsLog(dir.file("ops.log")).last_ts;
    ASSERT_GT(last_ts_before, uint64_t(0));

    {
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr);
        for (uint64_t i = 900001; i <= 900100; ++i) {
            ASSERT_EQ(e->Purchase(i, i, 777, 1), Result::Ok);
        }
    }

    ops_scan::OpsResult ops = ops_scan::ScanOpsLog(dir.file("ops.log"));
    EXPECT_TRUE(ops.ts_strictly_increasing) << "重启后写的记录 ts 和历史记录冲突了";
    EXPECT_GT(ops.last_ts, last_ts_before);

    auto e3 = Engine::Open(c);
    ASSERT_TRUE(e3 != nullptr);
    rt::ExpectMatchesOpsLog(*e3, dir);
    auto r = e3->DumpAll(Consistency::Fresh);
    EXPECT_EQ(rt::CountOf(r.entries, 777), int64_t(100));
}

}  // namespace
