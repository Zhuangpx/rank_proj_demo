// 用例公用的小工具：临时目录、测试用默认配置、榜单与对账结果的比对。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ops_scan.h"
#include "rank/engine.h"

namespace rt {

namespace fs = std::filesystem;

// 每个用例一个独立目录，用完删掉。目录名带自增序号，重复执行（--gtest_repeat）也不会撞。
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static std::atomic<uint64_t> seq{0};
        fs::path base = fs::temp_directory_path() / "rank_demo_tests";
        p_ = (base / (tag + "_" + std::to_string(seq.fetch_add(1)))).string();
        std::error_code ec;
        fs::remove_all(p_, ec);
        fs::create_directories(p_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return p_; }
    std::string        file(const char* name) const { return (fs::path(p_) / name).string(); }
    uint64_t           size_of(const char* name) const {
        std::error_code ec;
        auto            n = fs::file_size(fs::path(p_) / name, ec);
        return ec ? 0 : static_cast<uint64_t>(n);
    }
    bool exists(const char* name) const {
        std::error_code ec;
        return fs::exists(fs::path(p_) / name, ec);
    }

private:
    std::string p_;
};

// 测试默认：关 fsync（真 fsync 一次十几毫秒，整套用例会跑成几十分钟），
// 不攒批窗口，刷新周期调短。需要验落盘语义的用例自己把 enable_fsync 打开。
inline rank::Config TestConfig(const TempDir& dir) {
    rank::Config c;
    c.data_dir = dir.path();
    c.enable_fsync = false;
    c.batch_window_us = 0;
    c.t_max_ms = 50;
    c.t_min_ms = 5;
    c.expect_orders = 4096;
    return c;
}

inline std::map<uint64_t, int64_t> ToMap(const std::vector<rank::ItemCount>& v) {
    std::map<uint64_t, int64_t> m;
    for (const auto& e : v) {
        if (e.count != 0) m[e.item_id] = e.count;
    }
    return m;
}

// 榜单里某个物品的计数，不在榜上就算 0。
inline int64_t CountOf(const std::vector<rank::ItemCount>& v, uint64_t item_id) {
    for (const auto& e : v) {
        if (e.item_id == item_id) return e.count;
    }
    return 0;
}

// 引擎算出来的全量榜单，和对账工具按字节读 ops.log 加出来的结果，必须逐项相等。
// 这是两条独立的路径：一条经过内存合并与快照发布，一条只认文件内容。
inline void ExpectMatchesOpsLog(rank::Engine& engine, const TempDir& dir) {
    rank::TopKResult all = engine.DumpAll(rank::Consistency::Fresh);
    auto             from_engine = ToMap(all.entries);
    auto             from_file = ops_scan::NonZero(ops_scan::ScanOpsLog(dir.file("ops.log")).counts);
    EXPECT_EQ(from_engine.size(), from_file.size()) << "上榜物品数对不上";
    for (const auto& kv : from_file) {
        auto it = from_engine.find(kv.first);
        if (it == from_engine.end()) {
            ADD_FAILURE() << "物品 " << kv.first << " 在 ops.log 里算出 " << kv.second
                          << "，榜单里没有";
        } else {
            EXPECT_EQ(it->second, kv.second) << "物品 " << kv.first << " 计数不一致";
        }
    }
}

// rank.dat 的含义是「ts <= ts_merged 的记录全部算进去之后的榜单」。
// 按这个定义去对：不能用「ts_merged 不晚于末条记录」当判据——切表时刻的戳和记录
// 用的是同一个发号器，干净退出时最后一次刷新就发生在最后一条记录之后，
// ts_merged 比末条 ts 大是正常状态。
inline void ExpectRankDatMatchesOpsLog(const TempDir& dir) {
    ops_scan::RankDatResult rd = ops_scan::ReadRankDat(dir.file("rank.dat"));
    ASSERT_TRUE(rd.exists);
    ASSERT_TRUE(rd.valid);
    ops_scan::OpsResult ops = ops_scan::ScanOpsLog(dir.file("ops.log"), rd.ts_merged);
    EXPECT_EQ(ops_scan::NonZero(rd.counts), ops_scan::NonZero(ops.counts_upto));
}

// 订单表求和（INV-1）：订单表里还没退的那些，按物品加起来，必须等于榜单。
inline void ExpectMatchesOrderTable(rank::Engine& engine) {
    rank::TopKResult all = engine.DumpAll(rank::Consistency::Fresh);
    auto             from_rank = ToMap(all.entries);
    auto             from_orders = ToMap(engine.OrderTableSum());
    EXPECT_EQ(from_rank, from_orders) << "榜单与订单表求和不一致（INV-1）";
}

inline uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline void SleepMs(uint64_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 拉起崩溃注入程序。路径由 CMake 在编译时塞进来，避免测试去猜构建目录结构。
// 整条命令外面再包一层引号：cmd.exe 看到命令以引号开头时，会把开头那个引号
// 和整行里最后一个引号去掉，包一层正好让里面的引号原样留下。
// out_file 非空时把它的输出接到文件里，免得混进测试输出。
inline int RunCrashHelper(const std::string& args, const std::string& out_file = std::string()) {
    std::string redirect = out_file.empty() ? "" : " > \"" + out_file + "\" 2>&1";
    std::string cmd = "\"\"" + std::string(RANK_CRASH_HELPER_EXE) + "\" " + args + redirect + "\"";
    return std::system(cmd.c_str());
}

// 拉起压测程序，输出重定向到文件。重定向符号也必须包在同一层引号里，
// 否则被去掉的「最后一个引号」会是输出路径的那个。
inline int RunBench(const std::string& args, const std::string& out_file) {
    std::string cmd = "\"\"" + std::string(RANK_BENCH_EXE) + "\" " + args + " > \"" + out_file +
                      "\" 2>&1\"";
    return std::system(cmd.c_str());
}

inline std::string ReadTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 取出压测程序最后那行 CHECKSUM=... 的内容，没找到返回空串。
inline std::string ExtractChecksumLine(const std::string& text) {
    size_t pos = text.rfind("CHECKSUM=");
    if (pos == std::string::npos) return std::string();
    size_t end = text.find_first_of("\r\n", pos);
    return text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

}  // namespace rt
