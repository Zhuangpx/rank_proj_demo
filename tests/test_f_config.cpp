// 模块 F：配置与可移植性（F-01 ~ F-06）
// F-07（内存占用）和 F-08（MSVC / MinGW 两套编译器各构建一次）靠命令行跑，不在这里。
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

using rank::Consistency;
using rank::Engine;
using rank::Result;

namespace {

// 跑一轮固定的买卖，返回最终全量计数。所有 F 用例都用同一组操作，
// 这样换参数之后结果必须一模一样——不一致就是参数影响到了语义。
std::map<uint64_t, int64_t> RunFixedLoad(Engine& e, uint64_t n, uint64_t items) {
    for (uint64_t i = 1; i <= n; ++i) {
        if (e.Purchase(i, i, i % items + 1, static_cast<int64_t>(i % 5 + 1)) != Result::Ok) {
            ADD_FAILURE() << "第 " << i << " 笔购买失败";
        }
        if (i % 4 == 0 && e.Refund(i) != Result::Ok) ADD_FAILURE() << "第 " << i << " 笔回退失败";
    }
    return rt::ToMap(e.DumpAll(Consistency::Fresh).entries);
}

// F-01：所有时间与容量参数都压到最小，跑得通、结果对。
TEST(F_Config, F01_AllParamsAtMinimum) {
    rt::TempDir  dir("f01");
    rank::Config c;
    c.data_dir = dir.path();
    c.enable_fsync = false;
    c.t_max_ms = 0;          // 刷新线程几乎不休息
    c.t_min_ms = 0;          // Fresh 读不受最小间隔限制
    c.n_pending_max = 1;     // 攒到一条就刷
    c.batch_size = 1;        // 一条一批
    c.batch_window_us = 0;
    c.queue_cap = 1;         // 队列只有一个位置
    c.order_shards = 1;
    c.item_shards = 1;
    c.k_max = 1;
    c.rank_dat_period_ms = 0;  // 每次刷新都重写 rank.dat
    c.expect_orders = 1;

    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);
    auto got = RunFixedLoad(*e, 2000, 50);
    EXPECT_FALSE(got.empty());

    // k_max = 1：只能要一条，要两条就该被拒
    auto r1 = e->TopK(1, Consistency::Fresh);
    EXPECT_EQ(r1.code, Result::Ok);
    EXPECT_EQ(r1.entries.size(), size_t(1));
    EXPECT_EQ(e->TopK(2, Consistency::Fresh).code, Result::InvalidArg);

    rt::ExpectMatchesOpsLog(*e, dir);
    rt::ExpectMatchesOrderTable(*e);
}

// F-02：关掉文件写入就是纯内存模式，一个文件都不该出现。
TEST(F_Config, F02_PureMemoryMode) {
    rt::TempDir  dir("f02");
    rank::Config c = rt::TestConfig(dir);
    c.enable_file_io = false;
    c.enable_fsync = false;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    auto got = RunFixedLoad(*e, 5000, 200);
    EXPECT_FALSE(got.empty());
    EXPECT_FALSE(dir.exists("ops.log")) << "关掉文件写入还是建了 ops.log";
    EXPECT_FALSE(dir.exists("rank.dat")) << "关掉文件写入还是建了 rank.dat";
    EXPECT_FALSE(dir.exists("rank.dat.tmp"));

    // 内存里的语义一点不变：幂等、净计数、订单表求和都要对
    EXPECT_EQ(e->Purchase(1, 1, 1, 1), Result::AlreadyPurchased);
    rt::ExpectMatchesOrderTable(*e);
    EXPECT_EQ(e->GetMetrics().fsync_count, uint64_t(0));

    // 纯内存模式下重开引擎就是空的，这是预期行为，不是 bug
    auto e2 = Engine::Open(c);
    ASSERT_TRUE(e2 != nullptr);
    EXPECT_EQ(e2->DumpAll(Consistency::Fresh).entries.size(), size_t(0));
}

// F-03：订单表分片数只影响并发度，不影响结果。
TEST(F_Config, F03_OrderShardsDoNotChangeResult) {
    std::map<uint64_t, int64_t> ref;
    for (size_t shards : {size_t(1), size_t(2), size_t(64)}) {
        rt::TempDir  dir("f03_" + std::to_string(shards));
        rank::Config c = rt::TestConfig(dir);
        c.order_shards = shards;
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr) << "分片数 " << shards;

        auto got = RunFixedLoad(*e, 5000, 300);
        rt::ExpectMatchesOpsLog(*e, dir);
        if (ref.empty()) {
            ref = got;
            ASSERT_FALSE(ref.empty());
        } else {
            EXPECT_EQ(got, ref) << "订单表分片数 = " << shards << " 时结果不一样";
        }
    }
}

// F-04：待刷新表分片数同理。
TEST(F_Config, F04_ItemShardsDoNotChangeResult) {
    std::map<uint64_t, int64_t> ref;
    for (size_t shards : {size_t(1), size_t(3), size_t(8)}) {
        rt::TempDir  dir("f04_" + std::to_string(shards));
        rank::Config c = rt::TestConfig(dir);
        c.item_shards = shards;
        auto e = Engine::Open(c);
        ASSERT_TRUE(e != nullptr) << "分片数 " << shards;

        auto got = RunFixedLoad(*e, 5000, 300);
        rt::ExpectMatchesOpsLog(*e, dir);
        if (ref.empty()) {
            ref = got;
            ASSERT_FALSE(ref.empty());
        } else {
            EXPECT_EQ(got, ref) << "待刷新表分片数 = " << shards << " 时结果不一样";
        }
    }
}

// F-05：k_max 改小之后，边界就跟着改。
TEST(F_Config, F05_KMaxBoundary) {
    rt::TempDir  dir("f05");
    rank::Config c = rt::TestConfig(dir);
    c.k_max = 10;
    auto e = Engine::Open(c);
    ASSERT_TRUE(e != nullptr);

    for (uint64_t i = 1; i <= 50; ++i) ASSERT_EQ(e->Purchase(i, i, i, 1), Result::Ok);

    auto ok = e->TopK(10, Consistency::Fresh);
    EXPECT_EQ(ok.code, Result::Ok);
    EXPECT_EQ(ok.entries.size(), size_t(10));

    auto bad = e->TopK(11, Consistency::Fresh);
    EXPECT_EQ(bad.code, Result::InvalidArg);
    EXPECT_EQ(bad.entries.size(), size_t(0));

    // DumpAll 不受 k_max 限制，对账用得上
    EXPECT_EQ(e->DumpAll(Consistency::Fresh).entries.size(), size_t(50));
}

// F-06：压测入口同一个种子跑两次，校验和必须相同；换种子必须不同。
// 跑的是真正的 rank_bench 可执行文件，顺带也验证了它能被正常拉起来。
TEST(F_Config, F06_BenchIsReproducibleBySeed) {
    rt::TempDir dir("f06");
    const std::string common =
        " --threads 4 --ops 20000 --items 500 --refund-ratio 0.25 --no-fsync --tmax 20";

    std::string sums[3];
    const char* seeds[3] = {"7", "7", "8"};
    for (int i = 0; i < 3; ++i) {
        std::string data = dir.path() + "/run" + std::to_string(i);
        std::string out = dir.path() + "/out" + std::to_string(i) + ".txt";
        int rc = rt::RunBench("--dir \"" + data + "\" --seed " + seeds[i] + common, out);
        ASSERT_EQ(rc, 0) << "第 " << i << " 次压测退出码 " << rc << "\n" << rt::ReadTextFile(out);
        sums[i] = rt::ExtractChecksumLine(rt::ReadTextFile(out));
        ASSERT_FALSE(sums[i].empty()) << "第 " << i << " 次压测没输出 CHECKSUM 行";
    }

    EXPECT_EQ(sums[0], sums[1]) << "同种子两次结果不一致：\n" << sums[0] << "\n" << sums[1];
    EXPECT_NE(sums[0], sums[2]) << "换了种子结果还一样，说明种子没起作用";
}

}  // namespace
