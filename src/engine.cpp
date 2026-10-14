#include "rank/engine.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "counters.h"
#include "flusher.h"
#include "op_log.h"
#include "order_store.h"
#include "rank_store.h"
#include "recovery.h"
#include "staging.h"
#include "util.h"

namespace fs = std::filesystem;

namespace rank {

struct Engine::Impl {
    // cfg 要排在最前面：后面几个部件都拿着它的引用，得先于它们构造。
    Config          cfg;
    MetricsCounters counters;
    OrderStore      orders;
    RankStore       rank;
    StagingBuffer   staging;
    OpLog           oplog;
    Flusher         flusher;
    bool            started = false;

    explicit Impl(const Config& c)
        : cfg(c),
          orders(c.order_shards, c.expect_orders),
          rank(cfg, counters),
          staging(c.item_shards),
          oplog(cfg, staging, counters),
          flusher(cfg, staging, rank, counters) {}

    ~Impl() {
        if (!started) return;
        // 先停刷新线程：它退出前会再刷一次并写 rank.dat，这时日志线程还活着，能正常交接。
        flusher.Stop();
        oplog.Stop();
    }
};

Engine::Engine() = default;

Engine::~Engine() = default;

std::unique_ptr<Engine> Engine::Open(const Config& cfg) {
    std::unique_ptr<Engine> e(new Engine());
    e->impl_ = std::make_unique<Impl>(cfg);
    Impl& im = *e->impl_;

    if (im.cfg.enable_file_io) {
        std::error_code ec;
        fs::create_directories(im.cfg.data_dir, ec);
    }

    RecoveryResult rr = Recover(im.cfg, im.orders, im.rank);
    if (!rr.ok) return nullptr;

    // 先把时间戳发号器接上历史，再发第一份快照：这份快照的 ts_merged 要从
    // NextTs() 取一个「现在」，不能直接用 rr.ts_merged。
    // 理由是恢复扫完的那一刻，文件里的每一条记录都已经算进计数了，所以榜单其实
    // 已经合并到「现在」；而 rr.ts_merged 可能还是 rank.dat 里的旧值，空目录启动
    // 时甚至是 0——读请求拿到的陈旧度就会是从 1970 年算起的那个巨大数字。
    // 日志线程这时还没启动，中间不可能插进来新记录，所以这个戳一定大于已有的
    // 每一条 ts、又小于之后写的每一条 ts，恢复时 ts > ts_merged 的判断照旧成立。
    if (!im.oplog.Open(rr.last_ts)) return nullptr;
    uint64_t ts0 = im.oplog.NextTs();

    // 恢复完先发一份快照，读请求不用等第一次刷新。
    auto snap = std::make_shared<Snapshot>();
    snap->version = 1;
    snap->ts_merged = ts0;
    snap->all = std::make_shared<const ItemCounts>(std::move(rr.counts));
    snap->top = std::make_shared<const ItemCounts>(BuildTop(*snap->all, im.cfg.k_max));
    im.rank.Publish(snap);
    im.counters.last_ts_merged.store(ts0, std::memory_order_relaxed);
    im.counters.snapshot_version.store(1, std::memory_order_relaxed);

    im.oplog.SetPendingFullHint([&im] { im.flusher.Nudge(); });
    im.oplog.Start();
    im.flusher.Start();
    im.started = true;
    return e;
}

Result Engine::Purchase(uint64_t order_id, uint64_t uid, uint64_t item_id, int64_t qty) {
    (void)uid;  // 不写进 ops.log，也不参与任何判断
    // ① 参数校验，不合法直接返回，连订单表都不碰（FR-4）。
    if (qty <= 0 || item_id == 0) return Result::InvalidArg;
    if (impl_->oplog.ReadOnly()) return Result::ReadOnly;

    // ② 幂等判断与占号在同一把分片锁里做完。
    OrderStore::Decision d = impl_->orders.TryPurchase(order_id, item_id, qty);
    if (d.code != Result::Ok) return d.code;

    // ③ 放锁之后才等落盘：fsync 是毫秒级，持锁等会把整个分片堵死。
    OpRecord rec;
    rec.order_id = order_id;
    rec.item_id = item_id;
    rec.delta = d.delta;
    Result r = impl_->oplog.Submit(&rec);
    if (r != Result::Ok) {
        // 落盘失败就把订单表改回去，否则订单表和 ops.log 对不上，INV-1 不成立。
        impl_->orders.RollbackPurchase(order_id);
        return r;
    }
    return Result::Ok;
}

Result Engine::Refund(uint64_t order_id) {
    if (impl_->oplog.ReadOnly()) return Result::ReadOnly;

    OrderStore::Decision d = impl_->orders.TryRefund(order_id);
    if (d.code != Result::Ok) return d.code;

    OpRecord rec;
    rec.order_id = order_id;
    rec.item_id = d.item_id;
    rec.delta = d.delta;  // 负数
    Result r = impl_->oplog.Submit(&rec);
    if (r != Result::Ok) {
        impl_->orders.RollbackRefund(order_id, -d.delta);
        return r;
    }
    return Result::Ok;
}

TopKResult Engine::TopK(size_t k, Consistency c) {
    TopKResult out;
    if (k > impl_->cfg.k_max) {
        out.code = Result::InvalidArg;
        return out;
    }
    if (c == Consistency::Fresh) {
        // 进来先记时间，再请求刷新，然后等到已发布快照的 ts_merged 超过这个时间。
        // 这份快照是 T 之后才切的表，而本次调用之前已经返回成功的操作，
        // 增量在 T 之前就进表了，必然被那次切走（INV-5）。
        uint64_t t = NowNs();
        impl_->flusher.RequestFresh();
        impl_->flusher.WaitPublishedAfter(t);
    }
    // 读路径到这里一把锁都没有：取一次指针，从里面拷前 k 条。
    std::shared_ptr<const Snapshot> s = impl_->rank.Get();
    out.version = s->version;
    out.ts_merged = s->ts_merged;
    size_t n = std::min(k, s->top->size());
    out.entries.assign(s->top->begin(), s->top->begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

TopKResult Engine::DumpAll(Consistency c) {
    TopKResult out;
    if (c == Consistency::Fresh) {
        uint64_t t = NowNs();
        impl_->flusher.RequestFresh();
        impl_->flusher.WaitPublishedAfter(t);
    }
    std::shared_ptr<const Snapshot> s = impl_->rank.Get();
    out.version = s->version;
    out.ts_merged = s->ts_merged;
    out.entries = *s->all;
    return out;
}

MetricsView Engine::GetMetrics() const { return impl_->counters.View(); }

std::vector<ItemCount> Engine::OrderTableSum() const { return impl_->orders.SumByItem(); }

bool Engine::IsReadOnly() const { return impl_->oplog.ReadOnly(); }

}  // namespace rank
