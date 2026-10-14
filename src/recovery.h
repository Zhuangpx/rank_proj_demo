#pragma once

#include <cstdint>
#include <vector>

#include "order_store.h"
#include "rank/config.h"
#include "rank/types.h"
#include "rank_store.h"

namespace rank {

struct RecoveryResult {
    bool     ok = true;
    uint64_t last_ts = 0;       // 接着往下发 ts 的起点
    uint64_t ts_merged = 0;     // 第一份快照的时间戳
    uint64_t records = 0;       // ops.log 里读到的记录数
    uint64_t replayed = 0;      // 其中需要补算进榜单的条数
    uint64_t truncated = 0;     // 尾部丢掉的字节数
    bool     rank_dat_used = false;
    std::vector<ItemCount> counts;  // 全量计数，按 item_id 升序
};

// 读 rank.dat、扫 ops.log、重建订单表与全量计数（FR-17）。
// 这一步跑完才允许启动日志线程和刷新线程。
RecoveryResult Recover(const Config& cfg, OrderStore& orders, const RankStore& rank);

}  // namespace rank
