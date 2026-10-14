// 对账工具：直接读 ops.log 和 rank.dat，把 delta 按 item_id 加起来，
// 得到「这两个文件说榜单应该是什么」。不依赖引擎里的任何代码。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ops_scan.h"

namespace {

void Usage() {
    std::printf(
        "用法: rank_check <数据目录> [选项]\n"
        "  --dump            打印每个物品的计数\n"
        "  --nonzero         配合 --dump，只打印计数不为 0 的\n"
        "  --order <id>      打印某个订单在 ops.log 里的全部记录\n"
        "  --records [n]     把 ops.log 按文本倒出来，n 省略或为 0 就全部\n"
        "  --from <n>        配合 --records，从第 n 条开始（从 0 数）\n"
        "  --rank-entries    把 rank.dat 里的条目按文本倒出来\n"
        "\n"
        "ops.log 和 rank.dat 是定长二进制，没有文件头也没有分隔符，\n"
        "文本编辑器打不开。要看内容就用 --records / --rank-entries。\n");
}

using ops_scan::JoinPath;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 2;
    }
    std::string dir = argv[1];
    bool dump = false, nonzero = false, has_order = false;
    bool records = false, rank_entries = false;
    uint64_t order_id = 0, rec_limit = 0, rec_from = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dump") dump = true;
        else if (a == "--nonzero") nonzero = true;
        else if (a == "--rank-entries") rank_entries = true;
        else if (a == "--records") {
            records = true;
            // 条数是可选的，后面跟的要是个数字才当参数吃掉
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                rec_limit = std::strtoull(argv[++i], nullptr, 10);
            }
        }
        else if (a == "--from" && i + 1 < argc) rec_from = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--order" && i + 1 < argc) {
            has_order = true;
            order_id = std::strtoull(argv[++i], nullptr, 10);
        } else {
            Usage();
            return 2;
        }
    }

    // 先读 rank.dat 拿到 ts_merged，再扫 ops.log，这样能顺手算出
    // 「只算到 ts_merged 为止」的那份计数，用来和 rank.dat 逐项对账。
    ops_scan::RankDatResult rd = ops_scan::ReadRankDat(JoinPath(dir, "rank.dat"));
    ops_scan::OpsResult ops = ops_scan::ScanOpsLog(JoinPath(dir, "ops.log"), rd.ts_merged);

    std::printf("== ops.log ==\n");
    if (!ops.exists) {
        std::printf("  文件不存在\n");
    } else {
        std::printf("  长度        : %llu 字节 (%s 32 的整数倍", (unsigned long long)ops.file_size,
                    ops.tail_bytes == 0 ? "是" : "不是");
        if (ops.tail_bytes != 0) std::printf("，多出 %llu 字节", (unsigned long long)ops.tail_bytes);
        std::printf(")\n");
        std::printf("  记录数      : %llu  (购买 %llu / 回退 %llu)\n", (unsigned long long)ops.records,
                    (unsigned long long)ops.purchases, (unsigned long long)ops.refunds);
        std::printf("  ts 严格递增 : %s\n", ops.ts_strictly_increasing ? "是" : "否");
        std::printf("  首条 ts     : %llu\n", (unsigned long long)ops.first_ts);
        std::printf("  末条 ts     : %llu\n", (unsigned long long)ops.last_ts);
        std::printf("  上榜物品数  : %llu (非零 %llu)\n", (unsigned long long)ops.counts.size(),
                    (unsigned long long)ops_scan::NonZero(ops.counts).size());
    }

    std::printf("== rank.dat ==\n");
    if (!rd.exists) {
        std::printf("  文件不存在\n");
    } else {
        std::printf("  长度        : %llu 字节 (期望 16 + 16 × %llu = %llu) -> %s\n",
                    (unsigned long long)rd.file_size, (unsigned long long)rd.entries,
                    (unsigned long long)(16 + rd.entries * 16), rd.valid ? "有效" : "对不上，应作废重算");
        std::printf("  ts_merged   : %llu\n", (unsigned long long)rd.ts_merged);
        if (rd.valid && ops.exists) {
            // ts_merged 比末条记录的 ts 大是正常的：切表时刻的戳和记录用同一个
            // 发号器，干净退出时最后一次刷新发生在最后一条记录之后。所以这里不能
            // 用「ts_merged <= 末条 ts」当判据，那会把正常状态判成异常。
            std::printf("  ts > ts_merged 的记录 : %llu 条（这些是还没算进 rank.dat 的）\n",
                        (unsigned long long)ops.records_after);
            auto expect = ops_scan::NonZero(ops.counts_upto);
            auto got = ops_scan::NonZero(rd.counts);
            bool same = (expect == got);
            std::printf("  rank.dat 的计数 == ops.log 中 ts <= ts_merged 的 delta 之和 : %s\n",
                        same ? "是" : "否（异常）");
            if (!same) {
                std::printf("    rank.dat 非零条目 %llu，ops.log 算出 %llu\n",
                            (unsigned long long)got.size(), (unsigned long long)expect.size());
                uint64_t shown = 0;
                for (const auto& kv : expect) {
                    auto it = got.find(kv.first);
                    if (it == got.end() || it->second != kv.second) {
                        std::printf("    物品 %llu: ops.log 算出 %lld，rank.dat 里是 %lld\n",
                                    (unsigned long long)kv.first, (long long)kv.second,
                                    it == got.end() ? 0LL : (long long)it->second);
                        if (++shown >= 10) { std::printf("    ...\n"); break; }
                    }
                }
            }
        }
    }

    if (has_order) {
        std::printf("== 订单 %llu 的记录 ==\n", (unsigned long long)order_id);
        auto recs = ops_scan::ScanOpsLogForOrder(JoinPath(dir, "ops.log"), order_id);
        for (const auto& r : recs) {
            std::printf("  ts=%llu item=%llu delta=%+lld\n", (unsigned long long)r.ts,
                        (unsigned long long)r.item_id, (long long)r.delta);
        }
        if (recs.empty()) std::printf("  （没有）\n");
    }

    if (records) {
        std::printf("== ops.log 的记录（第 %llu 条起", (unsigned long long)rec_from);
        if (rec_limit > 0) std::printf("，最多 %llu 条", (unsigned long long)rec_limit);
        std::printf("）==\n");
        // 「序号」两个字占 4 格，后面手动补到和 %-10llu 一样宽
        std::printf("  序号       %-22s %-14s %-10s %s\n", "ts", "order_id", "item_id", "delta");
        uint64_t shown = 0;
        ops_scan::ForEachRecord(
            JoinPath(dir, "ops.log"),
            [&](uint64_t idx, uint64_t ts, uint64_t oid, uint64_t item, int64_t delta) {
                if (idx < rec_from) return true;
                if (rec_limit > 0 && shown >= rec_limit) return false;
                std::printf("  %-10llu %-22llu %-14llu %-10llu %+lld\n", (unsigned long long)idx,
                            (unsigned long long)ts, (unsigned long long)oid,
                            (unsigned long long)item, (long long)delta);
                ++shown;
                return true;
            });
        if (shown == 0) std::printf("  （没有）\n");
    }

    if (rank_entries) {
        std::printf("== rank.dat 的条目 ==\n");
        if (!rd.exists) {
            std::printf("  文件不存在\n");
        } else if (!rd.valid) {
            std::printf("  文件长度对不上，整份作废\n");
        } else {
            std::printf("  ts_merged = %llu，共 %llu 条\n", (unsigned long long)rd.ts_merged,
                        (unsigned long long)rd.entries);
            for (const auto& kv : rd.counts) {
                std::printf("  item=%llu count=%lld\n", (unsigned long long)kv.first,
                            (long long)kv.second);
            }
        }
    }

    if (dump) {
        std::printf("== 按 ops.log 算出来的全量计数 ==\n");
        for (const auto& kv : ops.counts) {
            if (nonzero && kv.second == 0) continue;
            std::printf("  %llu %lld\n", (unsigned long long)kv.first, (long long)kv.second);
        }
    }
    return 0;
}
