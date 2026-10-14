#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "rank/types.h"

namespace rank {

struct OrderEntry {
    uint64_t item_id = 0;
    int64_t  qty = 0;  // > 0 已购买；== 0 已回退
};

// 订单表：order_id -> (item_id, qty)，全量常驻，不淘汰。
// 幂等的唯一保证点就在这里——查、判、写三步必须在同一把分片锁内连续做完。
class OrderStore {
public:
    struct Decision {
        Result   code = Result::Ok;
        uint64_t item_id = 0;
        int64_t  delta = 0;  // 购买 +qty，回退 -qty
    };

    OrderStore(size_t shards, size_t reserve_hint);

    // 下面两个返回 Ok 时，订单表已经被改过了；调用方必须保证落盘失败时回滚。
    Decision TryPurchase(uint64_t order_id, uint64_t item_id, int64_t qty);
    Decision TryRefund(uint64_t order_id);

    void RollbackPurchase(uint64_t order_id);
    void RollbackRefund(uint64_t order_id, int64_t qty);

    // 恢复时逐条重放，此时还没有别的线程，不加锁也可以，这里照常加，省得多一套路径。
    void Replay(uint64_t order_id, uint64_t item_id, int64_t delta);

    // INV-1 对账：把 qty > 0 的订单按 item_id 加起来，按 item_id 升序返回。
    std::vector<ItemCount> SumByItem() const;

    size_t Size() const;
    size_t ShardCount() const { return shards_.size(); }

private:
    // 按 cache line 对齐：不对齐的话相邻两片的 mutex 会落在同一条 cache line 上，
    // 两个线程锁的是不同的片也会互相拖慢。
    struct alignas(64) Shard {
        mutable std::mutex m;
        std::unordered_map<uint64_t, OrderEntry> tab;
    };

    // 分片键只能是 order_id：同一笔订单的购买和回退必须落在同一把锁上。
    size_t IndexOf(uint64_t order_id) const;

    std::vector<std::unique_ptr<Shard>> shards_;
    size_t mask_ = 0;
};

}  // namespace rank
