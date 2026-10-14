#pragma once

#include <memory>
#include <vector>

#include "rank/config.h"
#include "rank/metrics.h"
#include "rank/types.h"

namespace rank {

// 进程内排行榜引擎。构造即完成崩溃恢复，构造返回之后才接受提交。
// 线程安全：Purchase / Refund / TopK 可以被任意多个线程同时调用。
class Engine {
public:
    // 恢复失败（文件读不动等）返回 nullptr。
    static std::unique_ptr<Engine> Open(const Config& cfg);

    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // uid 只为对齐业务接口（FR-1），不写进 ops.log，也不参与任何判断。
    // 记录里靠 delta 的符号区分购买和回退，详见 DESIGN.md §3.2。
    Result Purchase(uint64_t order_id, uint64_t uid, uint64_t item_id, int64_t qty);
    Result Refund(uint64_t order_id);

    // k > k_max 返回 InvalidArg；上榜物品不足 k 个就返回实际条数（FR-7）。
    TopKResult TopK(size_t k, Consistency c = Consistency::Stale);

    // 测试与对账用：取全量计数（含 count == 0 的物品），不受 k_max 限制。
    TopKResult DumpAll(Consistency c = Consistency::Stale);

    MetricsView GetMetrics() const;

    // INV-1 对账用：遍历订单表，把还处于已购买状态的 qty 按 item_id 加起来。
    // 结果按 item_id 升序，不含计数为 0 的物品。
    std::vector<ItemCount> OrderTableSum() const;

    // 是否已经因为写盘失败转为只读（FR-15）。
    bool IsReadOnly() const;

private:
    Engine();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rank
