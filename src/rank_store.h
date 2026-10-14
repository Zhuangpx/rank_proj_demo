#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <version>

#include "counters.h"
#include "rank/config.h"
#include "rank/types.h"

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#define RANK_HAS_ATOMIC_SHARED_PTR 1
#include <atomic>
#else
#define RANK_HAS_ATOMIC_SHARED_PTR 0
#include <shared_mutex>
#endif

namespace rank {

using ItemCounts = std::vector<ItemCount>;

// 排行榜数据就是这一份快照，建好之后不再改。
// all / top 用共享指针而不是直接装 vector：两次刷新之间没有任何增量时，
// 新快照只是换了 ts_merged，直接把上一份的两个表接过来，不用重新拷 1e5 条。
struct Snapshot {
    uint64_t version = 0;
    uint64_t ts_merged = 0;
    std::shared_ptr<const ItemCounts> all;  // 全量计数，按 item_id 升序，含 count == 0 的物品
    std::shared_ptr<const ItemCounts> top;  // 前 K 名，count 降序、item_id 升序，不含 count == 0
};

// 两个都按 item_id 升序，归并成一个。O(M + D)。
std::vector<ItemCount> MergeSorted(const std::vector<ItemCount>& all,
                                   const std::vector<ItemCount>& deltas);

// 从全量计数里选出前 k 名。O(M) + O(k log k)。
std::vector<ItemCount> BuildTop(const std::vector<ItemCount>& all, size_t k);

class RankStore {
public:
    RankStore(const Config& cfg, MetricsCounters& m);

    std::shared_ptr<const Snapshot> Get() const;
    void Publish(std::shared_ptr<const Snapshot> s);

    // 读 rank.dat。文件缺失或长度对不上就返回 false，调用方全量重算。
    bool Load(std::vector<ItemCount>* counts, uint64_t* ts_merged) const;
    // 写 rank.dat：写 .tmp → fsync → 改名覆盖，整文件替换（FR-16）。
    bool Save(const Snapshot& s);

private:
    const Config&    cfg_;
    MetricsCounters& m_;
#if RANK_HAS_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const Snapshot>> published_;
#else
    mutable std::shared_mutex       mu_;
    std::shared_ptr<const Snapshot> published_;
#endif
};

}  // namespace rank
