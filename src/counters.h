#pragma once

#include <algorithm>
#include <atomic>

#include "rank/metrics.h"
#include "util.h"

namespace rank {

// 指标的内部存放处。写的人分散在日志线程和刷新线程，所以全用原子量；
// 这些值只用于观测，彼此之间不要求同一时刻一致，一律 relaxed。
struct MetricsCounters {
    std::atomic<uint64_t> ops_durable{0};
    std::atomic<int64_t>  delta_submitted{0};
    std::atomic<int64_t>  delta_applied{0};
    std::atomic<uint64_t> events_applied{0};
    std::atomic<uint64_t> flush_count{0};
    std::atomic<uint64_t> flush_ns_total{0};
    std::atomic<uint64_t> flush_ns_max{0};
    std::atomic<uint64_t> fsync_count{0};
    std::atomic<uint64_t> rank_dat_writes{0};
    std::atomic<uint64_t> last_ts_merged{0};
    std::atomic<uint64_t> snapshot_version{0};

    void RecordFlush(uint64_t ns) {
        flush_count.fetch_add(1, std::memory_order_relaxed);
        flush_ns_total.fetch_add(ns, std::memory_order_relaxed);
        uint64_t prev = flush_ns_max.load(std::memory_order_relaxed);
        while (ns > prev && !flush_ns_max.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
        }
    }

    MetricsView View() const {
        MetricsView v;
        v.ops_durable = ops_durable.load(std::memory_order_relaxed);
        v.delta_submitted = delta_submitted.load(std::memory_order_relaxed);
        v.delta_applied = delta_applied.load(std::memory_order_relaxed);
        v.events_applied = events_applied.load(std::memory_order_relaxed);
        v.flush_count = flush_count.load(std::memory_order_relaxed);
        v.flush_ns_total = flush_ns_total.load(std::memory_order_relaxed);
        v.flush_ns_max = flush_ns_max.load(std::memory_order_relaxed);
        v.fsync_count = fsync_count.load(std::memory_order_relaxed);
        v.rank_dat_writes = rank_dat_writes.load(std::memory_order_relaxed);
        v.last_ts_merged = last_ts_merged.load(std::memory_order_relaxed);
        v.snapshot_version = snapshot_version.load(std::memory_order_relaxed);
        uint64_t now = NowNs();
        v.staleness_ns = now > v.last_ts_merged ? now - v.last_ts_merged : 0;
        return v;
    }
};

}  // namespace rank
