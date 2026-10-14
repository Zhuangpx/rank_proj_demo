// 对账工具的读取逻辑：直接按字节解析两个数据文件，不依赖引擎里的任何代码，
// 这样「引擎算出来的榜单」和「文件里本该是什么」是两条独立的路径，对得上才有意义。
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace ops_scan {

inline uint64_t LoadLE64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

struct OpsResult {
    bool     exists = false;
    uint64_t file_size = 0;
    uint64_t records = 0;
    uint64_t tail_bytes = 0;      // 文件长度不是 32 的整数倍时多出来的那几个字节
    uint64_t first_ts = 0;
    uint64_t last_ts = 0;
    bool     ts_strictly_increasing = true;
    uint64_t purchases = 0;
    uint64_t refunds = 0;
    std::map<uint64_t, int64_t> counts;  // item_id -> delta 总和
    // 只累计 ts <= ts_bound 的那部分，用来和 rank.dat 里的计数对账：
    // rank.dat 的含义就是「ts <= ts_merged 的记录全部算进去之后的榜单」。
    std::map<uint64_t, int64_t> counts_upto;
    uint64_t records_after = 0;  // ts > ts_bound 的记录条数
};

// ops.log 里属于某个订单的记录，按文件顺序给出 (item_id, delta)。
struct OrderRecord {
    uint64_t ts = 0;
    uint64_t item_id = 0;
    int64_t  delta = 0;
};

// 每 32 字节一条 (ts, order_id, item_id, delta)，把 delta 按 item_id 全部加起来。
// ts_bound 传 rank.dat 里的 ts_merged 时，额外给出「只算到那一刻」的那份计数。
inline OpsResult ScanOpsLog(const std::string& path, uint64_t ts_bound = ~uint64_t(0)) {
    OpsResult r;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return r;
    r.exists = true;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    r.file_size = sz < 0 ? 0 : static_cast<uint64_t>(sz);
    r.tail_bytes = r.file_size % 32;

    std::vector<unsigned char> buf(32 * 4096);
    uint64_t prev_ts = 0;
    while (true) {
        size_t got = std::fread(buf.data(), 1, buf.size(), fp);
        size_t n = got / 32;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char* p = buf.data() + i * 32;
            uint64_t ts = LoadLE64(p);
            uint64_t order_id = LoadLE64(p + 8);
            uint64_t item_id = LoadLE64(p + 16);
            int64_t  delta = static_cast<int64_t>(LoadLE64(p + 24));
            if (r.records == 0) r.first_ts = ts;
            else if (ts <= prev_ts) r.ts_strictly_increasing = false;
            prev_ts = ts;
            r.last_ts = ts;
            r.counts[item_id] += delta;
            if (ts <= ts_bound) r.counts_upto[item_id] += delta;
            else ++r.records_after;
            if (delta >= 0) ++r.purchases; else ++r.refunds;
            (void)order_id;
            ++r.records;
        }
        if (got < buf.size()) break;
    }
    std::fclose(fp);
    return r;
}

// 按文件顺序把每条记录交给 fn，fn 返回 false 就停。
// 不一次性读进内存：上百万条的日志全装成结构体没必要，倒出来也是边看边滚。
// fn 的签名是 (序号, ts, order_id, item_id, delta)。
template <typename Fn>
inline void ForEachRecord(const std::string& path, Fn fn) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return;
    std::vector<unsigned char> buf(32 * 4096);
    uint64_t idx = 0;
    bool go = true;
    while (go) {
        size_t got = std::fread(buf.data(), 1, buf.size(), fp);
        size_t n = got / 32;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char* p = buf.data() + i * 32;
            if (!fn(idx, LoadLE64(p), LoadLE64(p + 8), LoadLE64(p + 16),
                    static_cast<int64_t>(LoadLE64(p + 24)))) {
                go = false;
                break;
            }
            ++idx;
        }
        if (got < buf.size()) break;
    }
    std::fclose(fp);
}

inline std::vector<OrderRecord> ScanOpsLogForOrder(const std::string& path, uint64_t order_id) {
    std::vector<OrderRecord> out;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return out;
    std::vector<unsigned char> buf(32 * 4096);
    while (true) {
        size_t got = std::fread(buf.data(), 1, buf.size(), fp);
        size_t n = got / 32;
        for (size_t i = 0; i < n; ++i) {
            const unsigned char* p = buf.data() + i * 32;
            if (LoadLE64(p + 8) == order_id) {
                out.push_back(OrderRecord{LoadLE64(p), LoadLE64(p + 16),
                                          static_cast<int64_t>(LoadLE64(p + 24))});
            }
        }
        if (got < buf.size()) break;
    }
    std::fclose(fp);
    return out;
}

struct RankDatResult {
    bool     exists = false;
    bool     valid = false;  // 长度是否等于 16 + 16 × 条目数
    uint64_t file_size = 0;
    uint64_t ts_merged = 0;
    uint64_t entries = 0;
    std::map<uint64_t, int64_t> counts;
};

inline RankDatResult ReadRankDat(const std::string& path) {
    RankDatResult r;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return r;
    r.exists = true;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    r.file_size = sz < 0 ? 0 : static_cast<uint64_t>(sz);
    if (r.file_size < 16) {
        std::fclose(fp);
        return r;
    }
    std::vector<unsigned char> buf(r.file_size);
    size_t got = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (got != buf.size()) return r;
    r.ts_merged = LoadLE64(buf.data());
    r.entries = LoadLE64(buf.data() + 8);
    if (r.file_size != 16 + r.entries * 16) return r;
    r.valid = true;
    for (uint64_t i = 0; i < r.entries; ++i) {
        const unsigned char* p = buf.data() + 16 + i * 16;
        r.counts[LoadLE64(p)] = static_cast<int64_t>(LoadLE64(p + 8));
    }
    return r;
}

inline std::string JoinPath(const std::string& dir, const char* name) {
    if (dir.empty()) return name;
    char c = dir.back();
    if (c == '/' || c == '\\') return dir + name;
    return dir + "/" + name;
}

// 去掉计数为 0 的物品，方便和订单表求和的结果直接比。
inline std::map<uint64_t, int64_t> NonZero(const std::map<uint64_t, int64_t>& in) {
    std::map<uint64_t, int64_t> out;
    for (const auto& kv : in) {
        if (kv.second != 0) out.insert(kv);
    }
    return out;
}

}  // namespace ops_scan
