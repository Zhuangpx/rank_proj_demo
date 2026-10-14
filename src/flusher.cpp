#include "flusher.h"

#include <algorithm>
#include <chrono>

#include "util.h"

namespace rank {

Flusher::Flusher(const Config& cfg, StagingBuffer& staging, RankStore& rank, MetricsCounters& m)
    : cfg_(cfg), staging_(staging), rank_(rank), m_(m) {}

Flusher::~Flusher() { Stop(); }

void Flusher::Start() {
    last_flush_ns_ = SteadyNs();
    last_save_ns_ = SteadyNs();
    thread_ = std::thread([this] { ThreadMain(); });
}

void Flusher::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        stop_ = true;
        wake_ = true;
    }
    cv_wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    cv_published_.notify_all();
}

void Flusher::RequestFresh() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 多个读者共用这一个标志和下面那个条件变量，所以同时来的 Fresh 读
        // 会被合成一次刷新（FR-10）。
        fresh_requested_ = true;
        wake_ = true;
    }
    cv_wake_.notify_one();
}

void Flusher::Nudge() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_wake_.notify_one();
}

bool Flusher::WaitPublishedAfter(uint64_t ts) {
    // 超时只是兜底：正常情况下一轮刷新之内就满足了。系统时钟被往回调过的话
    // ts_merged 可能一直追不上 ts，这里宁可返回旧快照也不能把读线程挂死。
    auto limit = std::chrono::milliseconds(std::max<uint64_t>(cfg_.t_max_ms * 5, 2000));
    std::unique_lock<std::mutex> lk(mu_);
    return cv_published_.wait_for(lk, limit, [&] {
        return stop_ || rank_.Get()->ts_merged > ts;
    });
}

void Flusher::ThreadMain() {
    const uint64_t t_max_ns = cfg_.t_max_ms * 1000000ULL;
    const uint64_t t_min_ns = cfg_.t_min_ms * 1000000ULL;
    const uint64_t save_ns = cfg_.rank_dat_period_ms * 1000000ULL;

    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (stop_) break;
            uint64_t now = SteadyNs();
            uint64_t due = last_flush_ns_ + t_max_ns;
            if (fresh_requested_) due = std::min(due, last_flush_ns_ + t_min_ns);
            if (due > now && !wake_) {
                cv_wake_.wait_for(lk, std::chrono::nanoseconds(due - now),
                                  [&] { return stop_ || wake_ || fresh_requested_; });
            }
            wake_ = false;
            if (stop_) break;
        }

        uint64_t now = SteadyNs();
        uint64_t since = now - last_flush_ns_;
        bool by_time = since >= t_max_ns;
        bool by_fresh;
        {
            std::lock_guard<std::mutex> lk(mu_);
            by_fresh = fresh_requested_ && since >= t_min_ns;
        }
        // 攒够了也刷，但同样受最小间隔约束，否则读 QPS 一高刷新次数就跟着涨。
        bool by_pending = staging_.PendingItems() >= cfg_.n_pending_max && since >= t_min_ns;

        if (by_time || by_fresh || by_pending) {
            FlushOnce(now - last_save_ns_ >= save_ns);
        }
    }

    // 退出前再刷一次：此时日志线程还活着，能正常交接。顺手把 rank.dat 写上，
    // 正常关机之后重启就只需要补算很短一段。
    FlushOnce(true);
}

void Flusher::FlushOnce(bool save_rank_dat) {
    uint64_t t0 = SteadyNs();
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 清标志必须排在取走增量之前：清了之后才去切表，
        // 所以在清标志之前进来的读者，它记的 T 一定早于这次的切表时刻，这一轮就能满足它；
        // 清标志之后进来的读者会重新置上标志，由下一轮负责。
        fresh_requested_ = false;
    }

    Handoff h;
    if (!staging_.Take(&h)) return;  // 日志线程已经退出了

    MaybeCrash(cfg_, CrashPoint::DuringFlush);

    size_t n = 0;
    for (const auto& shard : h.deltas) n += shard.size();

    auto prev = rank_.Get();
    auto snap = std::make_shared<Snapshot>();
    snap->version = prev->version + 1;
    snap->ts_merged = h.ts;

    if (n == 0) {
        snap->all = prev->all;
        snap->top = prev->top;
    } else {
        std::vector<ItemCount> d;
        d.reserve(n);
        int64_t sum = 0;
        for (const auto& shard : h.deltas) {
            for (const auto& kv : shard) {
                d.push_back(ItemCount{kv.first, kv.second});
                sum += kv.second;
            }
        }
        // 分片键是 hash(item_id)，同一个物品只会出现在一片里，所以排完不用去重。
        std::sort(d.begin(), d.end(),
                  [](const ItemCount& a, const ItemCount& b) { return a.item_id < b.item_id; });

        auto all = std::make_shared<ItemCounts>(MergeSorted(*prev->all, d));
        auto top = std::make_shared<ItemCounts>(BuildTop(*all, cfg_.k_max));
        snap->all = std::move(all);
        snap->top = std::move(top);

        m_.events_applied.fetch_add(n, std::memory_order_relaxed);
        m_.delta_applied.fetch_add(sum, std::memory_order_relaxed);
    }

    rank_.Publish(snap);
    m_.last_ts_merged.store(snap->ts_merged, std::memory_order_relaxed);
    m_.snapshot_version.store(snap->version, std::memory_order_relaxed);
    m_.RecordFlush(SteadyNs() - t0);

    {
        std::lock_guard<std::mutex> lk(mu_);
        last_flush_ns_ = SteadyNs();
    }
    cv_published_.notify_all();

    // 落盘频率与刷新频率解耦（FR-16）。
    if (save_rank_dat) {
        rank_.Save(*snap);
        last_save_ns_ = SteadyNs();
    }
}

}  // namespace rank
