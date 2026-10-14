#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rank {

// 注入式崩溃点（FR-19）。命中后立刻 std::_Exit，不做任何收尾。
enum class CrashPoint {
    None = 0,
    BeforeWriteOpLog,     // 订单表已改，还没往 ops.log 写
    AfterFsyncBeforeAck,  // 已经 fsync，还没投递增量、还没返回调用方
    DuringFlush,          // 增量已被刷新线程取走，还没算进榜单
    AfterTmpBeforeRename, // rank.dat.tmp 写完了，还没改名
};

const char* ToString(CrashPoint p);
bool ParseCrashPoint(const std::string& s, CrashPoint* out);

// 所有时间与容量参数都在这里，测试可以设成 0 或极小值（FR-20）。
struct Config {
    std::string data_dir = ".";

    // —— 刷新触发（FR-9）——
    uint64_t t_max_ms = 200;           // 距上次刷新超过这么久就刷
    uint64_t t_min_ms = 10;            // 两次由 Fresh 读触发的刷新之间的最小间隔
    size_t   n_pending_max = 10000;    // 待刷新物品数攒到这么多就刷

    // —— 查询 ——
    size_t   k_max = 1000;             // TopK 允许的最大 k

    // —— 操作日志（FR-15）——
    size_t   batch_size = 4096;        // 一批最多多少条
    // 一批最多等多久（微秒）。默认 0 = 不等：队列里有什么就写什么，
    // 写盘期间新来的操作自然堆成下一批，批量随压力自己长大。
    // 不要在 Windows 上把它设成几毫秒：条件变量的定时等待会被系统时钟节拍
    // （约 15.6 ms）向上取整，设 2000 实际等 15.6 ms，吞吐掉两个数量级。
    uint64_t batch_window_us = 0;
    size_t   queue_cap = 64 * 1024;    // 提交队列上限，满了提交线程等

    // —— 排行榜文件（FR-16）——
    uint64_t rank_dat_period_ms = 1000;  // 落盘周期，与刷新频率解耦

    // —— 分片 ——
    size_t   order_shards = 64;        // 订单表分片数，向上取到 2 的幂
    size_t   item_shards = 8;          // 待刷新表分片数，向上取到 2 的幂

    // —— 开关 ——
    bool enable_file_io = true;        // 关掉就是纯内存路径，两个文件都不碰
    bool enable_fsync = true;          // 只关 fsync，照常写文件

    // —— 崩溃注入 ——
    CrashPoint crash_point = CrashPoint::None;
    uint64_t   crash_skip = 0;         // 前几次命中不崩，第 crash_skip+1 次才崩

    // —— 预留 ——
    size_t   expect_orders = 1 << 20;  // 启动时按这个给订单表预留桶，躲开 rehash
};

}  // namespace rank
