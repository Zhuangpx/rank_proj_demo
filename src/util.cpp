#include "util.h"

#include <array>
#include <atomic>
#include <cstdlib>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "rank/types.h"

namespace rank {

size_t RoundUpPow2(size_t v) {
    if (v <= 1) return 1;
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

bool SyncFile(std::FILE* f) {
    if (f == nullptr) return false;
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return ::_commit(::_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

void MaybeCrash(const Config& cfg, CrashPoint p) {
    if (cfg.crash_point != p || p == CrashPoint::None) return;
    // 前 crash_skip 次放过，第 crash_skip + 1 次才真的崩。
    static std::array<std::atomic<uint64_t>, 8> hits{};
    size_t idx = static_cast<size_t>(p) % hits.size();
    if (hits[idx].fetch_add(1) < cfg.crash_skip) return;
    // _Exit 不跑析构、不刷 stdio 缓冲，最接近「进程被拔电」。
    std::_Exit(42);
}

const char* ToString(Result r) {
    switch (r) {
        case Result::Ok: return "OK";
        case Result::AlreadyPurchased: return "ALREADY_PURCHASED";
        case Result::AlreadyRefunded: return "ALREADY_REFUNDED";
        case Result::OrderNotFound: return "ORDER_NOT_FOUND";
        case Result::InvalidArg: return "INVALID_ARG";
        case Result::IoError: return "IO_ERROR";
        case Result::ReadOnly: return "READ_ONLY";
        case Result::Shutdown: return "SHUTDOWN";
    }
    return "?";
}

const char* ToString(CrashPoint p) {
    switch (p) {
        case CrashPoint::None: return "none";
        case CrashPoint::BeforeWriteOpLog: return "before_write";
        case CrashPoint::AfterFsyncBeforeAck: return "after_fsync";
        case CrashPoint::DuringFlush: return "during_flush";
        case CrashPoint::AfterTmpBeforeRename: return "before_rename";
    }
    return "?";
}

bool ParseCrashPoint(const std::string& s, CrashPoint* out) {
    const CrashPoint all[] = {CrashPoint::None, CrashPoint::BeforeWriteOpLog,
                              CrashPoint::AfterFsyncBeforeAck, CrashPoint::DuringFlush,
                              CrashPoint::AfterTmpBeforeRename};
    for (CrashPoint p : all) {
        if (s == ToString(p)) {
            *out = p;
            return true;
        }
    }
    return false;
}

}  // namespace rank
