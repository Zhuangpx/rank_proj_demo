#pragma once

#include <cstdint>
#include <vector>

namespace rank {

// 接口返回码（FR-4）。Ok 与 Already* 都算业务成功。
enum class Result {
    Ok = 0,
    AlreadyPurchased,
    AlreadyRefunded,
    OrderNotFound,
    InvalidArg,
    IoError,   // 写盘失败：这笔没生效，订单表已回滚，引擎转只读（FR-15）
    ReadOnly,  // 引擎已经是只读状态，这笔连订单表都没碰
    Shutdown,  // 引擎正在关闭
};

const char* ToString(Result r);

enum class Consistency {
    Stale,  // 直接读当前快照，不触发刷新
    Fresh,  // 触发一次刷新并等它做完
};

struct ItemCount {
    uint64_t item_id = 0;
    int64_t  count = 0;
};

struct TopKResult {
    Result code = Result::Ok;
    std::vector<ItemCount> entries;
    uint64_t version = 0;    // 快照版本号；单次返回的条目必定同版本（INV-8）
    uint64_t ts_merged = 0;  // 这份榜单更新到哪个时间点（纳秒，系统时钟）
};

}  // namespace rank
