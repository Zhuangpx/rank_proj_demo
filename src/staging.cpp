#include "staging.h"

#include "util.h"

namespace rank {

StagingBuffer::StagingBuffer(size_t shards) {
    size_t n = RoundUpPow2(shards == 0 ? 1 : shards);
    mask_ = n - 1;
    cur_.resize(n);
}

void StagingBuffer::Add(uint64_t item_id, int64_t delta) {
    // 分片键必须是 hash(item_id)：同一个物品只会落在同一片，
    // 刷新时各片之间不会出现同一个 item_id，归并起来不用再去重。
    auto& tab = cur_[static_cast<size_t>(Mix64(item_id)) & mask_];
    auto [it, inserted] = tab.try_emplace(item_id, 0);
    it->second += delta;
    if (inserted) pending_items_.fetch_add(1, std::memory_order_relaxed);
}

void StagingBuffer::ServiceSwitch(const std::function<uint64_t()>& stamp) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!requested_ || ready_) return;
    }
    // 换整张表，不是遍历清零：换完日志线程往新表写，刷新线程读旧表，两边不碰同一张。
    std::vector<std::unordered_map<uint64_t, int64_t>> taken(cur_.size());
    cur_.swap(taken);
    pending_items_.store(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(m_);
    handoff_.deltas = std::move(taken);
    handoff_.ts = stamp();
    requested_ = false;
    ready_ = true;
    cv_done_.notify_one();
}

bool StagingBuffer::Take(Handoff* out) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (shutdown_) return false;
        requested_ = true;
    }
    // 叫醒必须放在 m_ 之外：回调要去拿日志线程的队列锁，
    // 在这儿持着 m_ 调用就成了「先 m_ 后队列锁」，而日志线程是反着来的。
    // 另外这个顺序也决定了不会丢唤醒：requested_ 先置上，日志线程醒来后
    // 无论是先看队列还是先看请求，都一定能看到它。
    if (wake_) wake_();

    std::unique_lock<std::mutex> lk(m_);
    cv_done_.wait(lk, [&] { return ready_ || shutdown_; });
    if (!ready_) return false;
    *out = std::move(handoff_);
    handoff_ = Handoff{};
    ready_ = false;
    return true;
}

void StagingBuffer::Shutdown() {
    std::lock_guard<std::mutex> lk(m_);
    shutdown_ = true;
    cv_done_.notify_all();
}

}  // namespace rank
