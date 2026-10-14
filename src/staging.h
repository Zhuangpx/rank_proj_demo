#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rank {

// 刷新线程一次取走的东西。
struct Handoff {
    std::vector<std::unordered_map<uint64_t, int64_t>> deltas;  // 按 item 分片
    uint64_t ts = 0;  // 切表那一刻的时间戳，应用完写进 ts_merged
};

// 待刷新表：item_id -> delta，同一个物品的多次操作合成一个数。
// 加法可交换，所以不需要保序；写的人只有日志线程一个，表里存普通 int64_t。
class StagingBuffer {
public:
    explicit StagingBuffer(size_t shards);

    // ——— 以下只许日志线程调用 ———
    void Add(uint64_t item_id, int64_t delta);
    size_t PendingItems() const { return pending_items_.load(std::memory_order_relaxed); }
    // 每批操作之间调用一次：有切表请求就换表，没有就立刻返回。
    // stamp() 由日志线程提供，返回切表时刻的时间戳（见 op_log.cpp 里为什么由它来取）。
    void ServiceSwitch(const std::function<uint64_t()>& stamp);
    // 日志线程退出前调用，叫醒还卡在 Take() 上的刷新线程。
    void Shutdown();
    // 日志线程启动前登记：刷新线程提出切表请求时用它把日志线程叫起来。
    // 回调里只许 notify，不许去拿日志线程的队列锁以外的锁，否则两把锁会套起来。
    void SetWakeLogThread(std::function<void()> f) { wake_ = std::move(f); }

    // ——— 以下只许刷新线程调用 ———
    // 置切表请求，等日志线程把整张表交过来。日志线程已经退出时返回 false。
    bool Take(Handoff* out);

    size_t ShardCount() const { return cur_.size(); }

private:
    mutable std::mutex m_;
    std::condition_variable cv_done_;
    bool requested_ = false;
    bool ready_ = false;
    bool shutdown_ = false;
    Handoff handoff_;

    // 只有日志线程碰，不受 m_ 保护：换表是整张 swap 出去的，
    // 刷新线程拿到的是另一张表，两边从头到尾不会同时碰同一张。
    std::vector<std::unordered_map<uint64_t, int64_t>> cur_;
    std::atomic<size_t> pending_items_{0};
    size_t mask_ = 0;
    std::function<void()> wake_;
};

}  // namespace rank
