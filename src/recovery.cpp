#include "recovery.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <unordered_map>

#include "op_log.h"
#include "util.h"

namespace fs = std::filesystem;

namespace rank {

RecoveryResult Recover(const Config& cfg, OrderStore& orders, const RankStore& rank) {
    RecoveryResult rr;
    if (!cfg.enable_file_io) return rr;

    // ① 读 rank.dat。文件缺失或长度对不上就计数清空、ts_merged = 0，全量重算。
    std::vector<ItemCount> loaded;
    uint64_t ts_merged = 0;
    rr.rank_dat_used = rank.Load(&loaded, &ts_merged);
    if (!rr.rank_dat_used) {
        loaded.clear();
        ts_merged = 0;
    }

    std::unordered_map<uint64_t, int64_t> counts;
    counts.reserve(loaded.size() * 2 + 16);
    for (const auto& e : loaded) counts[e.item_id] = e.count;

    // ② 扫 ops.log。尾部可能留着一条没写完的记录，表现就是长度不是 32 的整数倍；
    // 这条必然没返回过成功，整条丢弃并把文件截断，不能只应用它的一部分。
    std::string path = JoinPath(cfg.data_dir, "ops.log");
    std::error_code ec;
    if (fs::exists(path, ec)) {
        auto size = static_cast<uint64_t>(fs::file_size(path, ec));
        if (ec) {
            rr.ok = false;
            return rr;
        }
        uint64_t aligned = size - size % kOpRecordSize;
        if (aligned != size) {
            fs::resize_file(path, aligned, ec);
            if (ec) {
                rr.ok = false;
                return rr;
            }
            rr.truncated = size - aligned;
        }

        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (fp == nullptr) {
            rr.ok = false;
            return rr;
        }
        const size_t kChunk = 4096;
        std::vector<unsigned char> buf(kChunk * kOpRecordSize);
        uint64_t last_ts = 0;
        while (true) {
            size_t got = std::fread(buf.data(), 1, buf.size(), fp);
            size_t cnt = got / kOpRecordSize;
            for (size_t i = 0; i < cnt; ++i) {
                OpRecord r = DecodeOpRecord(buf.data() + i * kOpRecordSize);
                // 订单表不在 rank.dat 里，所以每条都要无条件重建，一条都不能跳。
                orders.Replay(r.order_id, r.item_id, r.delta);
                if (r.ts > ts_merged) {
                    counts[r.item_id] += r.delta;  // 只有没算过的才补
                    ++rr.replayed;
                }
                last_ts = r.ts;
                ++rr.records;
            }
            if (got < buf.size()) break;
        }
        std::fclose(fp);

        if (rr.records > 0) {
            // 扫完这一刻，文件里的每一条都已经算进计数了。
            ts_merged = std::max(ts_merged, last_ts);
        }
        rr.last_ts = std::max(ts_merged, last_ts);
    } else {
        rr.last_ts = ts_merged;
    }

    rr.ts_merged = ts_merged;
    rr.counts.reserve(counts.size());
    for (const auto& kv : counts) rr.counts.push_back(ItemCount{kv.first, kv.second});
    std::sort(rr.counts.begin(), rr.counts.end(),
              [](const ItemCount& a, const ItemCount& b) { return a.item_id < b.item_id; });
    return rr;
}

}  // namespace rank
