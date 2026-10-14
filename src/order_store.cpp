#include "order_store.h"

#include <algorithm>

#include "util.h"

namespace rank {

OrderStore::OrderStore(size_t shards, size_t reserve_hint) {
    size_t n = RoundUpPow2(shards == 0 ? 1 : shards);
    mask_ = n - 1;
    shards_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto s = std::make_unique<Shard>();
        if (reserve_hint > 0) s->tab.reserve(reserve_hint / n + 1);
        shards_.push_back(std::move(s));
    }
}

size_t OrderStore::IndexOf(uint64_t order_id) const {
    return static_cast<size_t>(Mix64(order_id)) & mask_;
}

OrderStore::Decision OrderStore::TryPurchase(uint64_t order_id, uint64_t item_id, int64_t qty) {
    Shard& s = *shards_[IndexOf(order_id)];
    std::lock_guard<std::mutex> lk(s.m);
    // 查、判、写必须连着做完：拆开的话两个线程会同时查到「不存在」，
    // 然后各写一条 +qty 的记录，幂等就破了。
    auto it = s.tab.find(order_id);
    if (it != s.tab.end()) {
        // qty > 0 是已购买，qty == 0 是已回退，两种都不允许再购买（订单号不能复用）。
        return {Result::AlreadyPurchased, it->second.item_id, 0};
    }
    s.tab.emplace(order_id, OrderEntry{item_id, qty});
    return {Result::Ok, item_id, qty};
}

OrderStore::Decision OrderStore::TryRefund(uint64_t order_id) {
    Shard& s = *shards_[IndexOf(order_id)];
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.tab.find(order_id);
    if (it == s.tab.end()) return {Result::OrderNotFound, 0, 0};
    if (it->second.qty == 0) return {Result::AlreadyRefunded, it->second.item_id, 0};
    int64_t qty = it->second.qty;
    uint64_t item_id = it->second.item_id;
    it->second.qty = 0;  // 记录不删除：删了就等于订单号可以被复用
    return {Result::Ok, item_id, -qty};
}

void OrderStore::RollbackPurchase(uint64_t order_id) {
    Shard& s = *shards_[IndexOf(order_id)];
    std::lock_guard<std::mutex> lk(s.m);
    s.tab.erase(order_id);
}

void OrderStore::RollbackRefund(uint64_t order_id, int64_t qty) {
    Shard& s = *shards_[IndexOf(order_id)];
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.tab.find(order_id);
    if (it != s.tab.end()) it->second.qty = qty;
}

void OrderStore::Replay(uint64_t order_id, uint64_t item_id, int64_t delta) {
    Shard& s = *shards_[IndexOf(order_id)];
    std::lock_guard<std::mutex> lk(s.m);
    if (delta > 0) {
        s.tab[order_id] = OrderEntry{item_id, delta};
    } else {
        // 回退记录自带 item_id，即使对应的购买记录被截断掉了也能重建出一条已回退的订单。
        s.tab[order_id] = OrderEntry{item_id, 0};
    }
}

std::vector<ItemCount> OrderStore::SumByItem() const {
    std::unordered_map<uint64_t, int64_t> acc;
    for (const auto& sp : shards_) {
        std::lock_guard<std::mutex> lk(sp->m);
        for (const auto& kv : sp->tab) {
            if (kv.second.qty > 0) acc[kv.second.item_id] += kv.second.qty;
        }
    }
    std::vector<ItemCount> out;
    out.reserve(acc.size());
    for (const auto& kv : acc) out.push_back(ItemCount{kv.first, kv.second});
    std::sort(out.begin(), out.end(),
              [](const ItemCount& a, const ItemCount& b) { return a.item_id < b.item_id; });
    return out;
}

size_t OrderStore::Size() const {
    size_t n = 0;
    for (const auto& sp : shards_) {
        std::lock_guard<std::mutex> lk(sp->m);
        n += sp->tab.size();
    }
    return n;
}

}  // namespace rank
