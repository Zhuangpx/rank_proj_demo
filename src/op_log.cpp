#include "op_log.h"

#include <algorithm>
#include <chrono>

#include "util.h"

namespace rank {

void EncodeOpRecord(const OpRecord& r, unsigned char* out32) {
    StoreU64LE(out32 + 0, r.ts);
    StoreU64LE(out32 + 8, r.order_id);
    StoreU64LE(out32 + 16, r.item_id);
    StoreI64LE(out32 + 24, r.delta);
}

OpRecord DecodeOpRecord(const unsigned char* in32) {
    OpRecord r;
    r.ts = LoadU64LE(in32 + 0);
    r.order_id = LoadU64LE(in32 + 8);
    r.item_id = LoadU64LE(in32 + 16);
    r.delta = LoadI64LE(in32 + 24);
    return r;
}

OpLog::OpLog(const Config& cfg, StagingBuffer& staging, MetricsCounters& m)
    : cfg_(cfg), staging_(staging), m_(m) {}

OpLog::~OpLog() {
    Stop();
    if (fp_ != nullptr) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

bool OpLog::Open(uint64_t last_ts) {
    last_ts_.store(last_ts, std::memory_order_relaxed);
    if (!cfg_.enable_file_io) return true;
    std::string path = JoinPath(cfg_.data_dir, "ops.log");
    fp_ = std::fopen(path.c_str(), "ab");
    return fp_ != nullptr;
}

// ts 取「当前系统时钟」与「上一条 + 1」里的较大值，因此严格递增，
// 且与写进文件的先后顺序一致（FR-14）。切表时刻的 ts_merged 也从这里取：
// 这样已经写下去的记录全部 ts < ts_merged，之后写的全部 ts > ts_merged，
// 恢复时用 ts > ts_merged 判断「还没算进榜单」才不会多算也不会漏算。
uint64_t OpLog::NextTs() {
    uint64_t prev = last_ts_.load(std::memory_order_relaxed);
    uint64_t now = NowNs();
    uint64_t t = now > prev ? now : prev + 1;
    last_ts_.store(t, std::memory_order_relaxed);
    return t;
}

void OpLog::Start() {
    staging_.SetWakeLogThread([this] {
        std::lock_guard<std::mutex> lk(qm_);
        switch_pending_ = true;
        cv_nonempty_.notify_one();
    });
    thread_ = std::thread([this] { ThreadMain(); });
}

void OpLog::Stop() {
    {
        std::lock_guard<std::mutex> lk(qm_);
        if (stop_) return;
        stop_ = true;
    }
    cv_nonempty_.notify_all();
    cv_notfull_.notify_all();
    if (thread_.joinable()) thread_.join();
}

Result OpLog::Submit(OpRecord* rec) {
    PendingOp op;
    op.rec = *rec;
    {
        std::unique_lock<std::mutex> lk(qm_);
        // 队列满了就在这儿等（FR-15）：写得比落盘快时，压力反推回提交线程。
        cv_notfull_.wait(lk, [&] { return q_.size() < cfg_.queue_cap || stop_; });
        if (stop_) return Result::Shutdown;
        q_.push_back(&op);
    }
    cv_nonempty_.notify_one();
    op.done.acquire();  // 等自己这一条落盘
    *rec = op.rec;      // 把日志线程填好的 ts 带回去
    return op.res;
}

void OpLog::ThreadMain() {
    using namespace std::chrono;
    auto stamp = [this] { return NextTs(); };

    while (true) {
        std::vector<PendingOp*> batch;
        {
            std::unique_lock<std::mutex> lk(qm_);
            // 切表请求也算一种「有活干」：一直没有写入时不能睡死在这儿，
            // 否则刷新线程的请求没人处理，Fresh 读等不到新快照（用例 C-09）。
            // 不用定时轮询，是因为 Windows 上的定时等待会被放大到 15 ms 以上。
            cv_nonempty_.wait(lk, [&] { return !q_.empty() || stop_ || switch_pending_; });
            switch_pending_ = false;  // 本轮末尾一定会调 ServiceSwitch，请求不会漏
            if (!q_.empty()) {
                // 攒批：等到够一批或者窗口到期，任一满足就落盘（FR-15）。
                // 窗口默认为 0，靠的是写盘本身的耗时自然攒批：这一批在写的时候
                // 新提交都堆在队列里，下一批就自动变大，不需要定时器。
                if (cfg_.batch_window_us > 0 && q_.size() < cfg_.batch_size && !stop_) {
                    auto dl = steady_clock::now() + microseconds(cfg_.batch_window_us);
                    cv_nonempty_.wait_until(
                        lk, dl, [&] { return q_.size() >= cfg_.batch_size || stop_; });
                }
                size_t n = std::min(q_.size(), cfg_.batch_size);
                batch.assign(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
                q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }

        if (!batch.empty()) {
            cv_notfull_.notify_all();
            ProcessBatch(batch);
            if (pending_full_hint_ && staging_.PendingItems() >= cfg_.n_pending_max) {
                pending_full_hint_();
            }
        }

        // 切表只能在这里做：此刻已经写下去的每一条记录，增量都已经进表了。
        staging_.ServiceSwitch(stamp);

        if (batch.empty()) {
            std::lock_guard<std::mutex> lk(qm_);
            if (stop_ && q_.empty()) break;
        }
    }
    staging_.Shutdown();
}

void OpLog::ProcessBatch(std::vector<PendingOp*>& batch) {
    buf_.resize(batch.size() * kOpRecordSize);
    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i]->rec.ts = NextTs();
        EncodeOpRecord(batch[i]->rec, buf_.data() + i * kOpRecordSize);
    }

    Result res = Result::Ok;
    MaybeCrash(cfg_, CrashPoint::BeforeWriteOpLog);

    if (cfg_.enable_file_io) {
        if (fp_ == nullptr) {
            res = Result::IoError;
        } else if (std::fwrite(buf_.data(), 1, buf_.size(), fp_) != buf_.size()) {
            res = Result::IoError;
        } else if (cfg_.enable_fsync) {
            if (!SyncFile(fp_)) {
                res = Result::IoError;
            } else {
                m_.fsync_count.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (std::fflush(fp_) != 0) {
            res = Result::IoError;
        }
    }

    if (res != Result::Ok) {
        // 写盘报错：这一批全部不 ack，引擎转只读（FR-15）。
        readonly_.store(true, std::memory_order_relaxed);
    }

    MaybeCrash(cfg_, CrashPoint::AfterFsyncBeforeAck);

    if (res == Result::Ok) {
        // 顺序不能换：fsync 返回之后才投递增量，否则会出现「榜单算了、文件里没有」。
        for (PendingOp* p : batch) {
            staging_.Add(p->rec.item_id, p->rec.delta);
            m_.delta_submitted.fetch_add(p->rec.delta, std::memory_order_relaxed);
        }
        m_.ops_durable.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    // 顺序不能换：投递完才叫醒提交线程，否则调用方拿到成功、紧接着 Fresh 读却看不到这笔（INV-5）。
    // release 之后 PendingOp 随时可能被提交线程的栈帧销毁，这里不能再碰 p。
    for (PendingOp* p : batch) {
        p->res = res;
        p->done.release();
    }
}

}  // namespace rank
