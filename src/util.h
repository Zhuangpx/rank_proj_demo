#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "rank/config.h"

namespace rank {

// ts 与 ts_merged 要跨重启可比（恢复时靠 ts > ts_merged 判断哪些没算过），
// 所以必须用系统时钟；重启后 steady_clock 会从头开始，拿它当 ts 会让恢复判断全错。
inline uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

// 「距上次刷新多久」这类间隔计时用稳定时钟，不受系统时钟被人调整的影响。
inline uint64_t SteadyNs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

inline void StoreU64LE(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xFF);
}

inline uint64_t LoadU64LE(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

inline void StoreI64LE(unsigned char* p, int64_t v) {
    StoreU64LE(p, static_cast<uint64_t>(v));
}

inline int64_t LoadI64LE(const unsigned char* p) {
    return static_cast<int64_t>(LoadU64LE(p));
}

// splitmix64 的收尾混合。order_id / item_id 在压测里通常是连续的，
// 直接取低位分片也能均匀，但混一下对任意输入都安全。
inline uint64_t Mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

size_t RoundUpPow2(size_t v);

// 把文件内容真正刷到盘上。MSVC 与 MinGW 都走 _commit，其它平台走 fsync。
bool SyncFile(std::FILE* f);

std::string JoinPath(const std::string& dir, const std::string& name);

// 命中注入的崩溃点就立刻终止进程，不做任何收尾（FR-19）。
void MaybeCrash(const Config& cfg, CrashPoint p);

}  // namespace rank
