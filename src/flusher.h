#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "counters.h"
#include "rank/config.h"
#include "rank_store.h"
#include "staging.h"

namespace rank {

// 更新排行榜的那个线程：判断该不该刷、取走增量、归并、发布快照、按周期写 rank.dat。
class Flusher {
public:
    Flusher(const Config& cfg, StagingBuffer& staging, RankStore& rank, MetricsCounters& m);
    ~Flusher();

    void Start();
    void Stop();

    // Fresh 读调用：请求刷新，并等到已发布快照的 ts_merged 超过 ts 为止。
    void RequestFresh();
    bool WaitPublishedAfter(uint64_t ts);

    // 待刷新物品数攒够了，日志线程叫一下。
    void Nudge();

private:
    void ThreadMain();
    void FlushOnce(bool force_save);

    const Config&    cfg_;
    StagingBuffer&   staging_;
    RankStore&       rank_;
    MetricsCounters& m_;

    std::mutex              mu_;
    std::condition_variable cv_wake_;       // 叫醒刷新线程
    std::condition_variable cv_published_;  // 通知等结果的 Fresh 读
    bool                    stop_ = false;
    bool                    wake_ = false;
    bool                    fresh_requested_ = false;
    uint64_t                last_flush_ns_ = 0;  // 稳定时钟

    uint64_t    last_save_ns_ = 0;  // 稳定时钟
    std::thread thread_;
};

}  // namespace rank
