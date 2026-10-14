#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <semaphore>
#include <thread>
#include <vector>

#include "counters.h"
#include "rank/config.h"
#include "rank/types.h"
#include "staging.h"

namespace rank {

// 一次购买或回退就是一条操作记录。delta 的符号就是操作类型，不另存字段。
struct OpRecord {
    uint64_t ts = 0;
    uint64_t order_id = 0;
    uint64_t item_id = 0;
    int64_t  delta = 0;  // 购买 +qty，回退 -qty
};

inline constexpr size_t kOpRecordSize = 32;  // 小端直存，无文件头

void     EncodeOpRecord(const OpRecord& r, unsigned char* out32);
OpRecord DecodeOpRecord(const unsigned char* in32);

// 提交队列 + ops.log 的唯一写入者。
class OpLog {
public:
    OpLog(const Config& cfg, StagingBuffer& staging, MetricsCounters& m);
    ~OpLog();

    // 恢复跑完之后调用：打开文件准备追加，并从 last_ts 接着往下发时间戳。
    bool Open(uint64_t last_ts);
    void Start();
    void Stop();

    // 阻塞到这一条落盘为止；rec->ts 由日志线程回填。
    Result Submit(OpRecord* rec);

    // 待刷新物品数攒够时叫一下刷新线程，省得它空等到 T_max。
    void SetPendingFullHint(std::function<void()> hint) { pending_full_hint_ = std::move(hint); }

    bool     ReadOnly() const { return readonly_.load(std::memory_order_relaxed); }
    uint64_t LastTs() const { return last_ts_.load(std::memory_order_relaxed); }

    // 时间戳只许从这里取。启动时发第一份快照也要取一个，见 engine.cpp 里的说明。
    uint64_t NextTs();

private:
    struct PendingOp {
        OpRecord rec;
        Result   res = Result::Ok;
        // 提交线程在自己这个信号量上等，不占队列锁。
        std::binary_semaphore done{0};
    };

    void ThreadMain();
    void ProcessBatch(std::vector<PendingOp*>& batch);

    const Config&   cfg_;
    StagingBuffer&  staging_;
    MetricsCounters& m_;

    std::mutex              qm_;
    std::condition_variable cv_nonempty_;
    std::condition_variable cv_notfull_;
    std::deque<PendingOp*>  q_;  // 只放指针：PendingOp 建在提交线程栈上，它要等到落盘才返回
    bool                    stop_ = false;
    // 刷新线程提了切表请求。和队列一样由 qm_ 保护，这样日志线程空等的时候
    // 不用靠定时轮询也能被叫醒——Windows 上定时等待会被系统时钟节拍放大到 15 ms 以上。
    bool                    switch_pending_ = false;

    std::thread           thread_;
    std::FILE*            fp_ = nullptr;
    std::atomic<uint64_t> last_ts_{0};
    std::atomic<bool>     readonly_{false};
    std::vector<unsigned char> buf_;
    std::function<void()> pending_full_hint_;
};

}  // namespace rank
