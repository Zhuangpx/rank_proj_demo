// 并发压测入口（FR-18）。线程数、操作总数、读写比、退单比例、物品数、
// 热点倾斜度、随机种子都可配；同一个种子跑两遍，最终榜单必定一样。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rank/engine.h"

namespace {

struct BenchArgs {
    size_t   threads = 4;
    uint64_t ops = 200000;      // 每个线程的操作数 = ops / threads
    double   read_ratio = 0.0;  // 这些操作里有多大比例是读
    double   fresh_ratio = 0.0; // 读里面有多大比例是 Fresh
    double   refund_ratio = 0.2;
    uint64_t items = 100000;
    double   zipf = 0.0;
    uint64_t seed = 1;
    size_t   k = 100;
};

double Pct(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    size_t idx = static_cast<size_t>(p * static_cast<double>(v.size() - 1));
    return static_cast<double>(v[idx]) / 1e6;  // 纳秒 -> 毫秒
}

// Zipf 采样：先把累积分布算出来，取随机数二分。α = 0 就是均匀分布。
class Zipf {
public:
    Zipf(uint64_t n, double alpha) : n_(n) {
        cdf_.resize(static_cast<size_t>(n));
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; ++i) {
            sum += 1.0 / std::pow(static_cast<double>(i), alpha);
            cdf_[static_cast<size_t>(i - 1)] = sum;
        }
        for (auto& v : cdf_) v /= sum;
    }
    uint64_t Sample(std::mt19937_64& rng) const {
        double u = static_cast<double>(rng() >> 11) / 9007199254740992.0;
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        size_t idx = static_cast<size_t>(it - cdf_.begin());
        if (idx >= cdf_.size()) idx = cdf_.size() - 1;
        return idx + 1;  // item_id 从 1 开始，0 是保留值
    }

private:
    uint64_t n_;
    std::vector<double> cdf_;
};

uint64_t Checksum(const std::vector<rank::ItemCount>& v) {
    uint64_t h = 1469598103934665603ULL;
    for (const auto& e : v) {
        if (e.count == 0) continue;
        h = (h ^ e.item_id) * 1099511628211ULL;
        h = (h ^ static_cast<uint64_t>(e.count)) * 1099511628211ULL;
    }
    return h;
}

void Usage() {
    std::printf(
        "用法: rank_bench [选项]\n"
        "  --dir <目录>          默认 bench_data\n"
        "  --threads <n>         提交线程数\n"
        "  --ops <n>             操作总数（含读）\n"
        "  --read-ratio <0..1>   读占比\n"
        "  --fresh-ratio <0..1>  读里面 Fresh 的占比\n"
        "  --refund-ratio <0..1> 写里面回退的占比\n"
        "  --items <n>           物品数\n"
        "  --zipf <a>            热点倾斜度，0 为均匀\n"
        "  --seed <n>\n"
        "  --k <n>               TopK 的 k\n"
        "  --no-fsync            照常写文件，但不 fsync\n"
        "  --no-file-io          完全不碰文件，纯内存路径\n"
        "  --tmax/--tmin/--npending/--window-us/--batch/--queue-cap/--kmax <n>\n"
        "  --order-shards/--item-shards <n>\n");
}

}  // namespace

int main(int argc, char** argv) {
    BenchArgs a;
    rank::Config cfg;
    cfg.data_dir = "bench_data";

    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto nx = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if (s == "--dir") cfg.data_dir = nx();
        else if (s == "--threads") a.threads = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--ops") a.ops = std::strtoull(nx(), nullptr, 10);
        else if (s == "--read-ratio") a.read_ratio = std::strtod(nx(), nullptr);
        else if (s == "--fresh-ratio") a.fresh_ratio = std::strtod(nx(), nullptr);
        else if (s == "--refund-ratio") a.refund_ratio = std::strtod(nx(), nullptr);
        else if (s == "--items") a.items = std::strtoull(nx(), nullptr, 10);
        else if (s == "--zipf") a.zipf = std::strtod(nx(), nullptr);
        else if (s == "--seed") a.seed = std::strtoull(nx(), nullptr, 10);
        else if (s == "--k") a.k = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--no-fsync") cfg.enable_fsync = false;
        else if (s == "--no-file-io") cfg.enable_file_io = false;
        else if (s == "--tmax") cfg.t_max_ms = std::strtoull(nx(), nullptr, 10);
        else if (s == "--tmin") cfg.t_min_ms = std::strtoull(nx(), nullptr, 10);
        else if (s == "--npending") cfg.n_pending_max = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--window-us") cfg.batch_window_us = std::strtoull(nx(), nullptr, 10);
        else if (s == "--batch") cfg.batch_size = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--queue-cap") cfg.queue_cap = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--kmax") cfg.k_max = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--order-shards") cfg.order_shards = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else if (s == "--item-shards") cfg.item_shards = static_cast<size_t>(std::strtoull(nx(), nullptr, 10));
        else { Usage(); return 2; }
    }
    if (a.threads == 0) a.threads = 1;
    if (a.items == 0) a.items = 1;
    if (a.k > cfg.k_max) a.k = cfg.k_max;

    auto engine = rank::Engine::Open(cfg);
    if (!engine) {
        std::fprintf(stderr, "引擎起不来，检查数据目录\n");
        return 3;
    }

    Zipf zipf(a.items, a.zipf);
    uint64_t per_thread = a.ops / a.threads;
    std::vector<std::vector<uint64_t>> lat(a.threads);
    std::atomic<uint64_t> writes{0}, reads{0}, write_ok{0}, refund_ok{0};

    auto t_start = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    ts.reserve(a.threads);
    for (size_t t = 0; t < a.threads; ++t) {
        ts.emplace_back([&, t] {
            std::mt19937_64 rng(a.seed + t * 0x9E3779B97F4A7C15ULL);
            std::vector<uint64_t> bought;
            bought.reserve(static_cast<size_t>(per_thread));
            lat[t].reserve(static_cast<size_t>(per_thread));
            uint64_t base = (static_cast<uint64_t>(t) + 1) << 40;
            uint64_t local_w = 0, local_r = 0, local_ok = 0, local_ref = 0;

            for (uint64_t i = 0; i < per_thread; ++i) {
                double u = static_cast<double>(rng() >> 11) / 9007199254740992.0;
                if (u < a.read_ratio) {
                    double u2 = static_cast<double>(rng() >> 11) / 9007199254740992.0;
                    engine->TopK(a.k, u2 < a.fresh_ratio ? rank::Consistency::Fresh
                                                         : rank::Consistency::Stale);
                    ++local_r;
                    continue;
                }
                double u3 = static_cast<double>(rng() >> 11) / 9007199254740992.0;
                auto c0 = std::chrono::steady_clock::now();
                if (u3 < a.refund_ratio && !bought.empty()) {
                    uint64_t victim = bought[rng() % bought.size()];
                    if (engine->Refund(victim) == rank::Result::Ok) ++local_ref;
                } else {
                    uint64_t oid = base + i;
                    uint64_t item = zipf.Sample(rng);
                    if (engine->Purchase(oid, oid, item, 1) == rank::Result::Ok) {
                        bought.push_back(oid);
                        ++local_ok;
                    }
                }
                auto c1 = std::chrono::steady_clock::now();
                lat[t].push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count()));
                ++local_w;
            }
            writes += local_w;
            reads += local_r;
            write_ok += local_ok;
            refund_ok += local_ref;
        });
    }
    for (auto& th : ts) th.join();
    auto t_end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t_end - t_start).count();

    // 等最后一次刷新做完，榜单才是最终值。
    rank::TopKResult fin = engine->DumpAll(rank::Consistency::Fresh);
    rank::MetricsView m = engine->GetMetrics();

    std::vector<uint64_t> all_lat;
    for (auto& v : lat) all_lat.insert(all_lat.end(), v.begin(), v.end());
    std::sort(all_lat.begin(), all_lat.end());

    std::printf("== 配置 ==\n");
    std::printf("  线程 %zu  操作总数 %llu  物品 %llu  Zipf %.2f  种子 %llu\n", a.threads,
                (unsigned long long)a.ops, (unsigned long long)a.items, a.zipf,
                (unsigned long long)a.seed);
    std::printf("  读占比 %.2f（其中 Fresh %.2f）  退单占比 %.2f\n", a.read_ratio, a.fresh_ratio,
                a.refund_ratio);
    std::printf("  fsync %s  文件写入 %s  T_max %llums  T_min %llums  批 %zu条/%lluus\n",
                cfg.enable_fsync ? "开" : "关", cfg.enable_file_io ? "开" : "关",
                (unsigned long long)cfg.t_max_ms, (unsigned long long)cfg.t_min_ms, cfg.batch_size,
                (unsigned long long)cfg.batch_window_us);

    std::printf("== 结果 ==\n");
    std::printf("  耗时            : %.3f s\n", secs);
    std::printf("  写操作          : %llu（成功购买 %llu，成功回退 %llu）\n",
                (unsigned long long)writes.load(), (unsigned long long)write_ok.load(),
                (unsigned long long)refund_ok.load());
    std::printf("  写吞吐          : %.0f ops/s\n", secs > 0 ? writes.load() / secs : 0.0);
    std::printf("  读操作          : %llu\n", (unsigned long long)reads.load());
    std::printf("  读吞吐          : %.0f ops/s\n", secs > 0 ? reads.load() / secs : 0.0);
    std::printf("  提交延迟 P50    : %.3f ms\n", Pct(all_lat, 0.50));
    std::printf("  提交延迟 P99    : %.3f ms\n", Pct(all_lat, 0.99));
    std::printf("  提交延迟 P99.9  : %.3f ms\n", Pct(all_lat, 0.999));
    std::printf("== 指标 ==\n");
    std::printf("  已落盘操作数    : %llu\n", (unsigned long long)m.ops_durable);
    std::printf("  刷新次数        : %llu\n", (unsigned long long)m.flush_count);
    std::printf("  刷新耗时 平均/最大: %.3f ms / %.3f ms\n", m.FlushAvgMs(),
                static_cast<double>(m.flush_ns_max) / 1e6);
    std::printf("  应用到榜单的事件: %llu\n", (unsigned long long)m.events_applied);
    std::printf("  合并比          : %.4f\n", m.CoalescingRatio());
    std::printf("  fsync 次数      : %llu\n", (unsigned long long)m.fsync_count);
    std::printf("  rank.dat 写入次数: %llu\n", (unsigned long long)m.rank_dat_writes);
    std::printf("  当前陈旧度      : %.3f ms\n", static_cast<double>(m.staleness_ns) / 1e6);
    std::printf("  delta 提交/应用 : %lld / %lld -> %s\n", (long long)m.delta_submitted,
                (long long)m.delta_applied,
                m.delta_submitted == m.delta_applied ? "一致" : "不一致（异常）");
    std::printf("== 榜单 ==\n");
    std::printf("  快照版本        : %llu\n", (unsigned long long)fin.version);
    std::printf("  上榜物品数      : %zu\n",
                std::count_if(fin.entries.begin(), fin.entries.end(),
                              [](const rank::ItemCount& e) { return e.count > 0; }));
    std::printf("  校验和          : %016llx （同种子两次运行必须相同）\n",
                (unsigned long long)Checksum(fin.entries));
    rank::TopKResult top = engine->TopK(std::min<size_t>(a.k, 10), rank::Consistency::Stale);
    for (size_t i = 0; i < top.entries.size(); ++i) {
        std::printf("  #%zu item=%llu count=%lld\n", i + 1,
                    (unsigned long long)top.entries[i].item_id, (long long)top.entries[i].count);
    }
    // 最后补一行纯 ASCII、固定格式的输出：用例 F-06 直接抓这一行做比对，
    // 不去解析上面那些给人看的中文字段，改排版不会把测试弄坏。
    std::printf("CHECKSUM=%016llx OPS=%llu DELTA=%lld\n", (unsigned long long)Checksum(fin.entries),
                (unsigned long long)writes.load(), (long long)m.delta_applied);
    return 0;
}
