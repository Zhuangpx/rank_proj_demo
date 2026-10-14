// 离线环境下的 GoogleTest 替身。
//
// 这台机器连不上网，FetchContent 拉不到 googletest，系统里也没有装。
// 所以这里实现 GoogleTest 对外接口的一个子集：TEST / TEST_F / EXPECT_* / ASSERT_* /
// GTEST_SKIP / InitGoogleTest / RUN_ALL_TESTS，以及 --gtest_filter、--gtest_repeat、
// --gtest_list_tests 这几个命令行开关。
//
// 测试源码里写的全是标准 GoogleTest 写法，一行都没有为这个替身改过。
// 以后机器能联网、或者系统装上了 googletest，CMake 会优先用真的那套，
// 测试代码不用动（见 tests/CMakeLists.txt 的三级回退）。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {
namespace internal {

// ——— 取值的可读表示 ———
template <typename T, typename = void>
struct IsStreamable : std::false_type {};
template <typename T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                            << std::declval<const T&>())>> : std::true_type {};

template <typename T>
std::string PrintValue(const T& v) {
    std::ostringstream os;
    if constexpr (IsStreamable<T>::value) {
        os << v;
    } else if constexpr (std::is_enum_v<T>) {
        os << static_cast<long long>(v);
    } else {
        os << "<" << sizeof(T) << "-byte object>";
    }
    return os.str();
}
inline std::string PrintValue(bool v) { return v ? "true" : "false"; }

// ——— 一次测试的状态 ———
struct RunState {
    bool        failed = false;
    bool        skipped = false;
    std::string skip_reason;
};
inline RunState& State() {
    static RunState s;
    return s;
}

class Msg {
public:
    template <typename T>
    Msg& operator<<(const T& v) {
        os_ << v;
        return *this;
    }
    std::string str() const { return os_.str(); }

private:
    std::ostringstream os_;
};

// 断言失败的落地点。写成 operator= 是为了让宏能接 << 补充说明：
//   EXPECT_EQ(a, b) << "上下文"  展开成  AssertHelper(...) = (Msg() << "上下文")
// 致命断言在宏里前面加 return，靠 operator= 返回 void 把测试函数直接结束掉。
class AssertHelper {
public:
    AssertHelper(const char* file, int line, bool fatal, std::string text)
        : file_(file), line_(line), fatal_(fatal), text_(std::move(text)) {}

    void operator=(const Msg& m) const {
        std::string extra = m.str();
        std::fprintf(stderr, "%s:%d: Failure\n%s\n", file_, line_, text_.c_str());
        if (!extra.empty()) std::fprintf(stderr, "  %s\n", extra.c_str());
        State().failed = true;
        (void)fatal_;
    }

private:
    const char* file_;
    int         line_;
    bool        fatal_;
    std::string text_;
};

class SkipHelper {
public:
    SkipHelper(const char* file, int line) : file_(file), line_(line) {}
    void operator=(const Msg& m) const {
        State().skipped = true;
        State().skip_reason = m.str();
        std::fprintf(stdout, "%s:%d: Skipped %s\n", file_, line_, m.str().c_str());
    }

private:
    const char* file_;
    int         line_;
};

struct CmpResult {
    bool        ok = true;
    std::string message;
    explicit operator bool() const { return ok; }
};

template <typename A, typename B>
std::string Describe(const char* ea, const char* eb, const char* op, const A& a, const B& b) {
    std::ostringstream os;
    os << "  期望: " << ea << " " << op << " " << eb << "\n"
       << "  实际: " << ea << " = " << PrintValue(a) << ", " << eb << " = " << PrintValue(b);
    return os.str();
}

#define GTEST_LITE_DEFINE_CMP(NAME, OP)                                                   \
    template <typename A, typename B>                                                     \
    CmpResult Cmp##NAME(const char* ea, const char* eb, const A& a, const B& b) {         \
        if (a OP b) return CmpResult{true, ""};                                           \
        return CmpResult{false, Describe(ea, eb, #OP, a, b)};                             \
    }
GTEST_LITE_DEFINE_CMP(EQ, ==)
GTEST_LITE_DEFINE_CMP(NE, !=)
GTEST_LITE_DEFINE_CMP(LT, <)
GTEST_LITE_DEFINE_CMP(LE, <=)
GTEST_LITE_DEFINE_CMP(GT, >)
GTEST_LITE_DEFINE_CMP(GE, >=)
#undef GTEST_LITE_DEFINE_CMP

inline CmpResult CmpBool(const char* expr, bool actual, bool expected) {
    if (actual == expected) return CmpResult{true, ""};
    std::ostringstream os;
    os << "  期望: " << expr << " 为 " << (expected ? "true" : "false") << "\n"
       << "  实际: " << (actual ? "true" : "false");
    return CmpResult{false, os.str()};
}

inline CmpResult CmpStr(const char* ea, const char* eb, const char* a, const char* b, bool eq) {
    bool same = (a == nullptr || b == nullptr) ? (a == b) : (std::strcmp(a, b) == 0);
    if (same == eq) return CmpResult{true, ""};
    std::ostringstream os;
    os << "  期望: " << ea << (eq ? " == " : " != ") << eb << "\n"
       << "  实际: \"" << (a ? a : "(null)") << "\" 与 \"" << (b ? b : "(null)") << "\"";
    return CmpResult{false, os.str()};
}

inline CmpResult CmpNear(const char* ea, const char* eb, double a, double b, double tol) {
    double d = a > b ? a - b : b - a;
    if (d <= tol) return CmpResult{true, ""};
    std::ostringstream os;
    os << "  期望: |" << ea << " - " << eb << "| <= " << tol << "\n"
       << "  实际: |" << a << " - " << b << "| = " << d;
    return CmpResult{false, os.str()};
}

}  // namespace internal

// ——— 测试基类 ———
class Test {
public:
    virtual ~Test() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
    void         Run() { TestBody(); }
    static bool  HasFailure() { return internal::State().failed; }
    static bool  HasFatalFailure() { return internal::State().failed; }

protected:
    virtual void TestBody() = 0;
};

namespace internal {

struct TestInfo {
    std::string               suite;
    std::string               name;
    std::function<Test*()>    factory;
};

inline std::vector<TestInfo>& Registry() {
    static std::vector<TestInfo> v;
    return v;
}

struct TestRegistrar {
    TestRegistrar(const char* suite, const char* name, std::function<Test*()> f) {
        Registry().push_back(TestInfo{suite, name, std::move(f)});
    }
};

struct Options {
    std::string filter = "*";
    int         repeat = 1;
    bool        list_only = false;
};
inline Options& Opts() {
    static Options o;
    return o;
}

// GoogleTest 的通配：* 任意多个字符，? 一个字符。
inline bool WildcardMatch(const char* pat, const char* str) {
    if (*pat == '\0') return *str == '\0';
    if (*pat == '*') return WildcardMatch(pat + 1, str) || (*str != '\0' && WildcardMatch(pat, str + 1));
    if (*str == '\0') return false;
    if (*pat == '?' || *pat == *str) return WildcardMatch(pat + 1, str + 1);
    return false;
}

inline std::vector<std::string> SplitColon(const std::string& s) {
    std::vector<std::string> out;
    std::string              cur;
    for (char c : s) {
        if (c == ':') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// filter 语法：正向模式[:正向模式...][-反向模式[:反向模式...]]
inline bool FilterMatch(const std::string& full) {
    const std::string& f = Opts().filter;
    size_t             dash = f.find('-');
    std::string        pos = dash == std::string::npos ? f : f.substr(0, dash);
    std::string        neg = dash == std::string::npos ? "" : f.substr(dash + 1);
    if (pos.empty()) pos = "*";

    bool matched = false;
    for (const auto& p : SplitColon(pos)) {
        if (!p.empty() && WildcardMatch(p.c_str(), full.c_str())) {
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    if (!neg.empty()) {
        for (const auto& p : SplitColon(neg)) {
            if (!p.empty() && WildcardMatch(p.c_str(), full.c_str())) return false;
        }
    }
    return true;
}

}  // namespace internal

inline void InitGoogleTest(int* argc, char** argv) {
    int w = 1;
    for (int i = 1; i < *argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--gtest_filter=", 0) == 0) {
            internal::Opts().filter = a.substr(15);
        } else if (a.rfind("--gtest_repeat=", 0) == 0) {
            internal::Opts().repeat = std::atoi(a.c_str() + 15);
        } else if (a == "--gtest_list_tests") {
            internal::Opts().list_only = true;
        } else if (a.rfind("--gtest_", 0) == 0) {
            // 其它 GoogleTest 开关（--gtest_color、--gtest_output 等）直接忽略
        } else {
            argv[w++] = argv[i];
        }
    }
    *argc = w;
}
inline void InitGoogleTest() {}

}  // namespace testing

inline int RUN_ALL_TESTS() {
    using namespace testing::internal;
    auto& reg = Registry();

    if (Opts().list_only) {
        std::string last;
        for (const auto& t : reg) {
            if (t.suite != last) {
                std::printf("%s.\n", t.suite.c_str());
                last = t.suite;
            }
            std::printf("  %s\n", t.name.c_str());
        }
        return 0;
    }

    std::vector<std::string> failed;
    int                      ran = 0, skipped = 0;
    for (int rep = 0; rep < Opts().repeat; ++rep) {
        if (Opts().repeat > 1) std::printf("[ REPEAT   ] 第 %d/%d 轮\n", rep + 1, Opts().repeat);
        for (const auto& t : reg) {
            std::string full = t.suite + "." + t.name;
            if (!FilterMatch(full)) continue;
            std::printf("[ RUN      ] %s\n", full.c_str());
            std::fflush(stdout);
            State() = RunState{};
            ++ran;

            testing::Test* obj = nullptr;
            try {
                obj = t.factory();
                obj->SetUp();
                if (!State().skipped) obj->Run();
                obj->TearDown();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "抛出了未捕获的异常: %s\n", e.what());
                State().failed = true;
            } catch (...) {
                std::fprintf(stderr, "抛出了未捕获的非标准异常\n");
                State().failed = true;
            }
            delete obj;

            if (State().failed) {
                failed.push_back(full);
                std::printf("[  FAILED  ] %s\n", full.c_str());
            } else if (State().skipped) {
                ++skipped;
                std::printf("[  SKIPPED ] %s\n", full.c_str());
            } else {
                std::printf("[       OK ] %s\n", full.c_str());
            }
            std::fflush(stdout);
        }
    }

    std::printf("[==========] 跑了 %d 个用例，跳过 %d 个，失败 %d 个\n", ran, skipped,
                static_cast<int>(failed.size()));
    for (const auto& f : failed) std::printf("[  FAILED  ] %s\n", f.c_str());
    std::fflush(stdout);
    return failed.empty() ? 0 : 1;
}

// ——— 宏 ———
#define GTEST_AMBIGUOUS_ELSE_BLOCKER_ \
    switch (0)                        \
    case 0:                           \
    default:

#define GTEST_LITE_NONFATAL_(text) \
    ::testing::internal::AssertHelper(__FILE__, __LINE__, false, text) = ::testing::internal::Msg()
#define GTEST_LITE_FATAL_(text) \
    return ::testing::internal::AssertHelper(__FILE__, __LINE__, true, text) = ::testing::internal::Msg()

// 两个操作数各只求值一次：断言里经常直接写 engine->Purchase(...)，求两次就错了。
#define GTEST_LITE_CMP_(NAME, a, b, FAILMACRO)                                             \
    GTEST_AMBIGUOUS_ELSE_BLOCKER_                                                          \
    if (const ::testing::internal::CmpResult gtest_r_ =                                    \
            ::testing::internal::Cmp##NAME(#a, #b, a, b))                                  \
        ;                                                                                  \
    else                                                                                   \
        FAILMACRO(gtest_r_.message)

#define EXPECT_EQ(a, b) GTEST_LITE_CMP_(EQ, a, b, GTEST_LITE_NONFATAL_)
#define EXPECT_NE(a, b) GTEST_LITE_CMP_(NE, a, b, GTEST_LITE_NONFATAL_)
#define EXPECT_LT(a, b) GTEST_LITE_CMP_(LT, a, b, GTEST_LITE_NONFATAL_)
#define EXPECT_LE(a, b) GTEST_LITE_CMP_(LE, a, b, GTEST_LITE_NONFATAL_)
#define EXPECT_GT(a, b) GTEST_LITE_CMP_(GT, a, b, GTEST_LITE_NONFATAL_)
#define EXPECT_GE(a, b) GTEST_LITE_CMP_(GE, a, b, GTEST_LITE_NONFATAL_)
#define ASSERT_EQ(a, b) GTEST_LITE_CMP_(EQ, a, b, GTEST_LITE_FATAL_)
#define ASSERT_NE(a, b) GTEST_LITE_CMP_(NE, a, b, GTEST_LITE_FATAL_)
#define ASSERT_LT(a, b) GTEST_LITE_CMP_(LT, a, b, GTEST_LITE_FATAL_)
#define ASSERT_LE(a, b) GTEST_LITE_CMP_(LE, a, b, GTEST_LITE_FATAL_)
#define ASSERT_GT(a, b) GTEST_LITE_CMP_(GT, a, b, GTEST_LITE_FATAL_)
#define ASSERT_GE(a, b) GTEST_LITE_CMP_(GE, a, b, GTEST_LITE_FATAL_)

#define GTEST_LITE_BOOL_(expr, expected, FAILMACRO)                                      \
    GTEST_AMBIGUOUS_ELSE_BLOCKER_                                                        \
    if (const ::testing::internal::CmpResult gtest_r_ =                                  \
            ::testing::internal::CmpBool(#expr, static_cast<bool>(expr), expected))      \
        ;                                                                                \
    else                                                                                 \
        FAILMACRO(gtest_r_.message)

#define EXPECT_TRUE(e) GTEST_LITE_BOOL_(e, true, GTEST_LITE_NONFATAL_)
#define EXPECT_FALSE(e) GTEST_LITE_BOOL_(e, false, GTEST_LITE_NONFATAL_)
#define ASSERT_TRUE(e) GTEST_LITE_BOOL_(e, true, GTEST_LITE_FATAL_)
#define ASSERT_FALSE(e) GTEST_LITE_BOOL_(e, false, GTEST_LITE_FATAL_)

#define GTEST_LITE_STR_(a, b, eq, FAILMACRO)                                             \
    GTEST_AMBIGUOUS_ELSE_BLOCKER_                                                        \
    if (const ::testing::internal::CmpResult gtest_r_ =                                  \
            ::testing::internal::CmpStr(#a, #b, a, b, eq))                               \
        ;                                                                                \
    else                                                                                 \
        FAILMACRO(gtest_r_.message)

#define EXPECT_STREQ(a, b) GTEST_LITE_STR_(a, b, true, GTEST_LITE_NONFATAL_)
#define EXPECT_STRNE(a, b) GTEST_LITE_STR_(a, b, false, GTEST_LITE_NONFATAL_)
#define ASSERT_STREQ(a, b) GTEST_LITE_STR_(a, b, true, GTEST_LITE_FATAL_)

#define GTEST_LITE_NEAR_(a, b, tol, FAILMACRO)                                           \
    GTEST_AMBIGUOUS_ELSE_BLOCKER_                                                        \
    if (const ::testing::internal::CmpResult gtest_r_ =                                  \
            ::testing::internal::CmpNear(#a, #b, a, b, tol))                             \
        ;                                                                                \
    else                                                                                 \
        FAILMACRO(gtest_r_.message)

#define EXPECT_NEAR(a, b, tol) GTEST_LITE_NEAR_(a, b, tol, GTEST_LITE_NONFATAL_)
#define ASSERT_NEAR(a, b, tol) GTEST_LITE_NEAR_(a, b, tol, GTEST_LITE_FATAL_)

#define ADD_FAILURE() GTEST_LITE_NONFATAL_("ADD_FAILURE()")
#define FAIL() GTEST_LITE_FATAL_("FAIL()")
#define SUCCEED() ((void)0)
#define GTEST_SKIP() \
    return ::testing::internal::SkipHelper(__FILE__, __LINE__) = ::testing::internal::Msg()

#define GTEST_LITE_TEST_(suite_name, test_name, parent_class)                            \
    class suite_name##_##test_name##_Test : public parent_class {                        \
    public:                                                                              \
        suite_name##_##test_name##_Test() = default;                                     \
                                                                                         \
    private:                                                                             \
        void TestBody() override;                                                        \
        static ::testing::internal::TestRegistrar gtest_registrar_;                      \
    };                                                                                   \
    ::testing::internal::TestRegistrar suite_name##_##test_name##_Test::gtest_registrar_(\
        #suite_name, #test_name,                                                         \
        [] { return static_cast<::testing::Test*>(new suite_name##_##test_name##_Test()); }); \
    void suite_name##_##test_name##_Test::TestBody()

#define TEST(suite_name, test_name) GTEST_LITE_TEST_(suite_name, test_name, ::testing::Test)
#define TEST_F(fixture, test_name) GTEST_LITE_TEST_(fixture, test_name, fixture)
