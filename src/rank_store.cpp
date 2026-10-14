#include "rank_store.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "util.h"

namespace fs = std::filesystem;

namespace rank {

namespace {
constexpr size_t kRankHeaderSize = 16;  // ts_merged(8) + 条目数(8)
constexpr size_t kRankEntrySize = 16;   // item_id(8) + count(8)
}  // namespace

std::vector<ItemCount> MergeSorted(const std::vector<ItemCount>& all,
                                   const std::vector<ItemCount>& deltas) {
    std::vector<ItemCount> out;
    out.reserve(all.size() + deltas.size());
    size_t i = 0, j = 0;
    while (i < all.size() && j < deltas.size()) {
        if (all[i].item_id < deltas[j].item_id) {
            out.push_back(all[i++]);
        } else if (deltas[j].item_id < all[i].item_id) {
            out.push_back(deltas[j++]);
        } else {
            out.push_back(ItemCount{all[i].item_id, all[i].count + deltas[j].count});
            ++i;
            ++j;
        }
    }
    while (i < all.size()) out.push_back(all[i++]);
    while (j < deltas.size()) out.push_back(deltas[j++]);
    return out;
}

std::vector<ItemCount> BuildTop(const std::vector<ItemCount>& all, size_t k) {
    auto cmp = [](const ItemCount& a, const ItemCount& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.item_id < b.item_id;  // 同分按 item_id 升序，排名必须确定（INV-7）
    };
    std::vector<ItemCount> t;
    t.reserve(std::min<size_t>(all.size(), 1 << 16));
    for (const auto& e : all) {
        if (e.count > 0) t.push_back(e);  // count == 0 不进榜（FR-7）
    }
    if (k == 0) return {};
    if (t.size() > k) {
        std::nth_element(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(k), t.end(), cmp);
        t.resize(k);
    }
    std::sort(t.begin(), t.end(), cmp);
    return t;
}

RankStore::RankStore(const Config& cfg, MetricsCounters& m) : cfg_(cfg), m_(m) {
    auto empty = std::make_shared<Snapshot>();
    empty->all = std::make_shared<const ItemCounts>();
    empty->top = std::make_shared<const ItemCounts>();
#if RANK_HAS_ATOMIC_SHARED_PTR
    published_.store(empty);
#else
    published_ = empty;
#endif
}

std::shared_ptr<const Snapshot> RankStore::Get() const {
#if RANK_HAS_ATOMIC_SHARED_PTR
    return published_.load(std::memory_order_acquire);
#else
    std::shared_lock<std::shared_mutex> lk(mu_);
    return published_;
#endif
}

void RankStore::Publish(std::shared_ptr<const Snapshot> s) {
    // 发布用 release、读用 acquire：读线程看见新指针时，快照内容必定已经写完。
#if RANK_HAS_ATOMIC_SHARED_PTR
    published_.store(std::move(s), std::memory_order_release);
#else
    std::unique_lock<std::shared_mutex> lk(mu_);
    published_ = std::move(s);
#endif
}

bool RankStore::Load(std::vector<ItemCount>* counts, uint64_t* ts_merged) const {
    counts->clear();
    *ts_merged = 0;
    if (!cfg_.enable_file_io) return false;

    std::string path = JoinPath(cfg_.data_dir, "rank.dat");
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    auto size = static_cast<size_t>(fs::file_size(path, ec));
    if (ec) return false;
    if (size < kRankHeaderSize) return false;

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) return false;
    std::vector<unsigned char> buf(size);
    size_t got = std::fread(buf.data(), 1, size, fp);
    std::fclose(fp);
    if (got != size) return false;

    *ts_merged = LoadU64LE(buf.data());
    uint64_t n = LoadU64LE(buf.data() + 8);
    // 条目数和文件长度对不上就整份作废，不做猜测性修补（FR-17）。
    if (size != kRankHeaderSize + static_cast<size_t>(n) * kRankEntrySize) {
        *ts_merged = 0;
        return false;
    }
    counts->resize(static_cast<size_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* p = buf.data() + kRankHeaderSize + i * kRankEntrySize;
        (*counts)[i].item_id = LoadU64LE(p);
        (*counts)[i].count = LoadI64LE(p + 8);
    }
    return true;
}

bool RankStore::Save(const Snapshot& s) {
    if (!cfg_.enable_file_io) return true;

    std::string path = JoinPath(cfg_.data_dir, "rank.dat");
    std::string tmp = path + ".tmp";

    const ItemCounts& all = *s.all;
    std::vector<unsigned char> buf(kRankHeaderSize + all.size() * kRankEntrySize);
    StoreU64LE(buf.data(), s.ts_merged);
    StoreU64LE(buf.data() + 8, static_cast<uint64_t>(all.size()));
    for (size_t i = 0; i < all.size(); ++i) {
        unsigned char* p = buf.data() + kRankHeaderSize + i * kRankEntrySize;
        StoreU64LE(p, all[i].item_id);
        StoreI64LE(p + 8, all[i].count);
    }

    std::FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (fp == nullptr) return false;
    bool ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
    if (ok) ok = cfg_.enable_fsync ? SyncFile(fp) : (std::fflush(fp) == 0);
    std::fclose(fp);
    if (!ok) return false;

    MaybeCrash(cfg_, CrashPoint::AfterTmpBeforeRename);

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // 个别平台的 rename 不肯覆盖已存在的文件，退一步先删后改名。
        // 这中间崩了就是没有 rank.dat，启动时全量重算，不影响正确性。
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) return false;
    }
    m_.rank_dat_writes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace rank
