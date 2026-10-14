#pragma once

#include <cstdint>

namespace rank {

// 对外的指标读数（FR-13）。取一次是一组当时的值，不保证彼此严格同一时刻。
struct MetricsView {
    uint64_t ops_durable = 0;      // 已落盘的操作数
    int64_t  delta_submitted = 0;  // 投进待刷新表的 delta 总和
    int64_t  delta_applied = 0;    // 已经算进榜单的 delta 总和（INV-9 对账用）
    uint64_t events_applied = 0;   // 应用到榜单的 (item, delta) 条目数
    uint64_t flush_count = 0;
    uint64_t flush_ns_total = 0;
    uint64_t flush_ns_max = 0;
    uint64_t fsync_count = 0;
    uint64_t rank_dat_writes = 0;
    uint64_t last_ts_merged = 0;   // 最近发布的快照更新到哪个时间点
    uint64_t staleness_ns = 0;     // 当前时间减去 last_ts_merged
    uint64_t snapshot_version = 0;

    // 合并比 = 应用到榜单的事件数 / 已落盘的操作数，越小合并效果越好。
    double CoalescingRatio() const {
        return ops_durable == 0 ? 0.0
                                : static_cast<double>(events_applied) / static_cast<double>(ops_durable);
    }
    double FlushAvgMs() const {
        return flush_count == 0 ? 0.0
                                : static_cast<double>(flush_ns_total) / static_cast<double>(flush_count) / 1e6;
    }
};

}  // namespace rank
