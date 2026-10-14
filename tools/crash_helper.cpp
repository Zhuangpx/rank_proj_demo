// 崩溃注入用的小程序：起一个引擎跑一段固定的负载，跑到指定位置就把进程干掉。
// 测试用 std::system 把它拉起来，然后检查数据目录重启后的状态（FR-19）。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rank/engine.h"

namespace {

void Usage() {
    std::printf(
        "用法: rank_crash_helper --dir <目录> [选项]\n"
        "  --crash <none|before_write|after_fsync|during_flush|before_rename>\n"
        "  --skip <n>            前 n 次命中不崩\n"
        "  --ops <n>             提交多少笔购买\n"
        "  --order-base <n>      订单号起点\n"
        "  --items <n>           物品数\n"
        "  --qty <n>             每笔数量\n"
        "  --refund-pct <0..100> 购买之后按这个比例回退之前的某一笔\n"
        "  --seed <n>\n"
        "  --exit-after-ms <n>   跑到第 n 毫秒直接终止进程（模拟随机被杀）\n"
        "  --tmax/--tmin/--window-us/--rank-period <n>\n"
        "  --final-fresh         结束前做一次 Fresh 读\n"
        "  --print-open          打印 Open（含崩溃恢复）的耗时，用例 D-12 用\n");
}

uint64_t ArgU64(const char* s) { return std::strtoull(s, nullptr, 10); }

}  // namespace

int main(int argc, char** argv) {
    rank::Config cfg;
    cfg.data_dir.clear();
    uint64_t ops = 100, order_base = 1, items = 16, qty = 1, refund_pct = 0, seed = 1;
    uint64_t exit_after_ms = 0;
    bool final_fresh = false;
    bool print_open = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (a == "--dir") cfg.data_dir = next();
        else if (a == "--crash") {
            if (!rank::ParseCrashPoint(next(), &cfg.crash_point)) { Usage(); return 2; }
        }
        else if (a == "--skip") cfg.crash_skip = ArgU64(next());
        else if (a == "--ops") ops = ArgU64(next());
        else if (a == "--order-base") order_base = ArgU64(next());
        else if (a == "--items") items = ArgU64(next());
        else if (a == "--qty") qty = ArgU64(next());
        else if (a == "--refund-pct") refund_pct = ArgU64(next());
        else if (a == "--seed") seed = ArgU64(next());
        else if (a == "--exit-after-ms") exit_after_ms = ArgU64(next());
        else if (a == "--tmax") cfg.t_max_ms = ArgU64(next());
        else if (a == "--tmin") cfg.t_min_ms = ArgU64(next());
        else if (a == "--window-us") cfg.batch_window_us = ArgU64(next());
        else if (a == "--rank-period") cfg.rank_dat_period_ms = ArgU64(next());
        else if (a == "--npending") cfg.n_pending_max = static_cast<size_t>(ArgU64(next()));
        else if (a == "--final-fresh") final_fresh = true;
        else if (a == "--print-open") print_open = true;
        else { Usage(); return 2; }
    }
    if (cfg.data_dir.empty()) { Usage(); return 2; }
    if (items == 0) items = 1;

    // 单独计时 Open：恢复就在 Open 里做完，用例 D-12 要的就是这个数。
    auto t_open = std::chrono::steady_clock::now();
    auto engine = rank::Engine::Open(cfg);
    double open_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t_open)
                         .count();
    if (!engine) {
        std::fprintf(stderr, "引擎起不来\n");
        return 3;
    }
    if (print_open) std::printf("open_ms=%.3f\n", open_ms);

    if (exit_after_ms > 0) {
        std::thread([exit_after_ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(exit_after_ms));
            std::_Exit(42);  // 模拟被拔电，不跑任何收尾
        }).detach();
    }

    std::mt19937_64 rng(seed);
    std::vector<uint64_t> bought;
    bought.reserve(static_cast<size_t>(ops));
    uint64_t acked = 0;
    for (uint64_t i = 0; i < ops; ++i) {
        uint64_t oid = order_base + i;
        uint64_t item = 1 + rng() % items;
        if (engine->Purchase(oid, oid, item, static_cast<int64_t>(qty)) == rank::Result::Ok) {
            ++acked;
            bought.push_back(oid);
        }
        if (refund_pct > 0 && !bought.empty() && rng() % 100 < refund_pct) {
            uint64_t victim = bought[rng() % bought.size()];
            if (engine->Refund(victim) == rank::Result::Ok) ++acked;
        }
    }

    if (final_fresh) engine->TopK(10, rank::Consistency::Fresh);
    std::printf("acked=%llu\n", (unsigned long long)acked);
    return 0;
}
