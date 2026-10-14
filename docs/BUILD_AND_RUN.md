# 构建与运行

## 1. 环境要求

| 项 | 要求 | 本机实测版本 |
|---|---|---|
| 编译器 | 支持 C++20 的 MSVC 或 GCC | MinGW-w64 GCC 15.2.0（x86_64-win32-seh） |
| CMake | ≥ 3.16 | 4.3.0-rc2 |
| GoogleTest | 可选，见第 6 节 | 没装，走自带的精简版 |

不需要任何数据库、消息队列或第三方库。源码树里唯一的平台分支在 `src/util.cpp` 的 `SyncFile()`：Windows 用 `<io.h>` 的 `_commit`，其他平台用 `<unistd.h>` 的 `fsync`。

## 2. 构建

MinGW：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

MSVC（本机没装，命令未实测）：

```bash
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

不写 `CMAKE_BUILD_TYPE` 时默认 Release。

构建开关：

| 开关 | 默认 | 说明 |
|---|---|---|
| `RANK_BUILD_TESTS` | ON | 编译 `rank_tests` |
| `RANK_BUILD_TOOLS` | ON | 编译 `rank_demo`、`rank_check`、`rank_crash_helper` |
| `RANK_BUILD_BENCH` | ON | 编译 `rank_bench` |
| `RANK_GTEST_FETCH` | OFF | 允许联网下载 GoogleTest |

测试要拉起 `rank_crash_helper`（崩溃用例）和 `rank_bench`（同种子复现用例），所以开着 `RANK_BUILD_TESTS` 就不能关掉另外两个，CMake 会直接报错拦住。

MinGW 下必须静态链接运行时（`CMakeLists.txt` 里已经写死 `-static -static-libgcc -static-libstdc++`）。机器上往往还装着 Git 自带的 mingw64，它的 `libstdc++-6.dll` / `libwinpthread-1.dll` 会先于编译器自己的被 `PATH` 找到；一旦混进来，exe 里内联的 win32 线程版 `mutex`/`condition_variable`/`semaphore` 和 DLL 里 posix 线程版的对不上，表现是信号量 `release` 了但 `acquire` 不醒，整个进程卡死。

## 3. 产物

| 路径 | 是什么 |
|---|---|
| `build/librank.a` | 引擎本体 |
| `build/rank_demo.exe` | 多用户并发买退的演示，按事件写日志 |
| `build/tests/rank_tests.exe` | 全部自动化用例 |
| `build/rank_bench.exe` | 压测入口 |
| `build/rank_check.exe` | 对账工具，直接按字节读 `ops.log` 和 `rank.dat` |
| `build/rank_crash_helper.exe` | 崩溃注入程序，供崩溃类用例拉起 |

## 4. 跑演示

```bash
# 默认：8 个用户同时对物品 42 反复买退，每人 40 次操作
./build/rank_demo.exe

# 人多一点、每人操作多一点、榜单更新更频繁
./build/rank_demo.exe --users 32 --ops 100 --tmax 20

# 换个目录放，日志单独指路径
./build/rank_demo.exe --dir /tmp/d1 --log /tmp/d1/run.log
```

`a` 个用户线程同时对**同一个**物品买和退，每做完一次操作，做这次操作的线程紧接着查一次排行榜。三类事件按发生顺序写进一个日志文件，默认是 `demo_data/demo.log`：

| 事件 | 一行里有什么 |
|---|---|
| 操作 | 时间戳、哪个用户、买还是退、订单号、件数、引擎的返回码、这次操作之后的真实净次数 |
| 查询 | 时间戳、榜单版本号、榜单内容、当时的真实净次数、榜单比真实数据落后多少 |
| 更新 | 时间戳、版本号怎么变的，以及更新前后的「真实数据 + 榜单」两行对比 |

时间戳是从开跑算起的毫秒数（`steady_clock`），不是墙上时间，方便看相对先后和间隔。

几个说明：

- **真实净次数**是 demo 自己数的（买成功 `+qty`，退成功 `−qty`），代表业务上到底卖了多少；榜单是引擎发布的快照，更新有延迟。两者的差就是延迟的大小，直接打在「查询」行末尾。
- 查询走 `Stale`，直接读已经发布的快照，不逼引擎刷新，否则就看不出延迟了。只有跑完最后对账那一次走 `Fresh`。
- 「操作」行上的真实净次数是**这次操作自己做完之后**的值，从原子加的返回值算出来，不是事后再读一次总数。多个线程同时加减时，日志里相邻几行的数字不一定递增——写日志的先后和加减的先后本来就是两回事。
- 更新事件是在查询里顺带发现的：每次查询比一下版本号，变大了就补记一条。没有单独的监视线程。版本号变小的那种忽略掉——查快照在锁外面做，两个线程可能拿到 v4 和 v5 却按相反顺序写日志，引擎的版本号本身只增不减。
- 它不是测试程序，不判定每一步对不对，只记录实际发生了什么。只有最后的汇总会对一次账：demo 自己数的、引擎订单表求和、强制更新之后的榜单，三条独立的路算出来的数必须一样，不一样进程返回 1。

每次跑之前数据目录会被清空，跑完留着，可以接着用 `rank_check` 查（见第 7 节）：

```bash
./build/rank_check.exe demo_data --dump --nonzero
```

日志文件写在数据目录里，和 `ops.log`、`rank.dat` 放一起。引擎只按文件名认那两个文件，不扫目录，所以多出来的 `demo.log` 不影响它启动和恢复。

全部选项见 `rank_demo --help`：

| 选项 | 默认 | 说明 |
|---|---|---|
| `--users <a>` | 8 | 同时操作的用户数 |
| `--ops <n>` | 40 | 每个用户做多少次操作 |
| `--item <id>` | 42 | 所有用户都操作这一个物品 |
| `--qty <n>` | 1 | 每次买卖几件 |
| `--refund-pct <p>` | 35 | 回退占比 |
| `--think-ms <n>` | 10 | 两次操作之间歇多久 |
| `--tmax <ms>` | 50 | 榜单最多多久更新一次 |
| `--seed <n>` | 1 | 随机种子 |
| `--fsync` | 关 | 每批都等真落盘，开了慢很多 |
| `--dir <目录>` | `demo_data` | 数据目录，跑之前会清空 |
| `--log <文件>` | `<数据目录>/demo.log` | 日志文件 |

`--think-ms` 在 Windows 上实际会被系统的定时精度（约 15.6 ms）往上取整，所以填 10 和填 15 跑出来差不多。想让一次更新里压进更多操作，就把 `--users` 调大或者 `--think-ms` 调成 0。

实跑出来的日志片段和汇总在 `README.md` 的「运行验证」一节。

## 5. 跑测试

```bash
# 全部用例
./build/tests/rank_tests.exe

# 走 ctest（整套算一个测试项，超时 900 秒）
ctest --test-dir build --output-on-failure

# 只跑某个模块
./build/tests/rank_tests.exe --gtest_filter='D_Persist.*'

# E-07：并发敏感的用例重复多轮
./build/tests/rank_tests.exe --gtest_repeat=50 --gtest_brief=1 \
  --gtest_filter='A_Submit.A10*:A_Submit.A11*:B_Query.B08*:B_Query.B09*:E_Concurrency.E0[123]*'
```

用例各自建临时目录（`%TEMP%/rank_demo_tests/<用例名>_<序号>`），跑完删掉，所以 `--gtest_repeat` 多轮也不会互相干扰。但**不要同时跑两个 `rank_tests` 进程**：目录名里的序号是进程内自增的，两个进程会撞到同一个目录。

每一版实际跑出来的结果记在 `docs/TEST_RESULTS.md`，半自动用例（压测、崩溃、启动耗时）的完整命令和数字都在那里，可以照着复跑。

## 6. GoogleTest 从哪来

`tests/CMakeLists.txt` 按三级找，配置时会打印选中的是哪一级：

1. 系统里装了 → `find_package(GTest)`；
2. 加了 `-DRANK_GTEST_FETCH=ON` 且能联网 → `FetchContent` 拉 v1.14.0；
3. 都不行 → 用仓库自带的 `tests/gtest_lite`。

第三级是一个只有头文件的精简版，实现了用例注册、`TEST`、`EXPECT_*`/`ASSERT_*`、`--gtest_filter`、`--gtest_repeat`、`--gtest_brief`、失败计数和退出码。断言语义和真 GoogleTest 一致，但没有 `TEST_F`、参数化用例、死亡测试、XML 报告。**本机的测试结果是在第三级下跑出来的**——这台机器连不上 googletest 的仓库（`git ls-remote` 失败），CMake 打印的是「GoogleTest: 用自带的 tests/gtest_lite（没装也没下载到）」。装了真 GoogleTest 的机器上重跑一遍更有说服力。

## 7. 压测

```bash
# 默认：8 线程、100 万次操作
./build/rank_bench.exe --dir bench_data

# E-04 的那一档：64 线程、200 万次操作、25% 退单、20% 读
./build/rank_bench.exe --dir bench_data --threads 64 --ops 2000000 \
  --refund-ratio 0.25 --read-ratio 0.2 --items 200000

# 纯内存，不碰文件
./build/rank_bench.exe --no-file-io --threads 16 --ops 1000000
```

全部选项见 `rank_bench --help`。几个常用的：

| 选项 | 说明 |
|---|---|
| `--threads <n>` | 提交线程数 |
| `--ops <n>` | 操作总数（含读） |
| `--read-ratio <0..1>` / `--fresh-ratio <0..1>` | 读占比 / 读里面 `Fresh` 的占比 |
| `--refund-ratio <0..1>` | 写里面回退的占比 |
| `--items <n>` / `--zipf <a>` | 物品数 / 热点倾斜度，0 为均匀 |
| `--seed <n>` | 随机种子，同种子同参数结果可复现 |
| `--no-fsync` / `--no-file-io` | 照常写文件但不 fsync / 完全不碰文件 |
| `--tmax/--tmin/--npending/--window-us/--batch/--queue-cap/--kmax` | 刷新与队列参数 |
| `--order-shards/--item-shards <n>` | 分片数 |

输出的最后一行是固定格式的 `CHECKSUM=... OPS=... DELTA=...`，纯 ASCII，用例 F-06 直接抓这一行比对同种子两次运行的结果。

压测跑完数据目录还在，可以接着对账：

```bash
./build/rank_check.exe bench_data
./build/rank_check.exe bench_data --dump --nonzero     # 打印每个物品的计数
./build/rank_check.exe bench_data --order 12345        # 某个订单的全部记录
```

`ops.log` 和 `rank.dat` 是定长二进制，没有文件头也没有分隔符，**文本编辑器打不开**（VS Code 会直接说是二进制文件然后拒绝显示）。要看里面的内容用这两个选项：

```bash
./build/rank_check.exe DIR --records              # 整份 ops.log 按文本倒出来
./build/rank_check.exe DIR --records 20           # 只看前 20 条
./build/rank_check.exe DIR --records 20 --from 1000   # 从第 1000 条起看 20 条
./build/rank_check.exe DIR --rank-entries         # rank.dat 里的条目
```

倒出来长这样，一行一条记录：

```
  序号       ts                     order_id       item_id    delta
  0          1791962356097431000    1001           1          +2
  3          1791962356109753000    1001           1          -2
```

序号 n 对应文件里第 `32 × n` 字节起的 32 个字节。同一个 `order_id` 出现两次、`delta` 一正一负，就是先买后退：文件里没有操作类型字段，退单靠 `delta` 取负表示；退单那条的 `item_id` 是按 `order_id` 从订单表查出来的，所以和购买那条一定相同。

`rank_check` 不碰引擎里的任何代码，只按字节解析两个文件，所以它和引擎是两条独立的路径，对得上才说明问题。它会检查：`ops.log` 长度是不是 32 的整数倍、`ts` 是不是严格递增、`rank.dat` 长度是不是等于 `16 + 16 × 条目数`、以及 `rank.dat` 里的计数是不是等于 `ops.log` 中 `ts ≤ ts_merged` 那部分 delta 的总和。

> `ts_merged` 比 `ops.log` 末条记录的 `ts` 大是**正常状态**，不是异常：切表时刻的戳和记录用的是同一个发号器，干净退出时最后一次刷新就发生在最后一条记录之后。

## 8. 崩溃注入

```bash
# 跑 1 万笔，刷新中途第 6 次命中时直接终止进程
./build/rank_crash_helper.exe --dir crash_data --crash during_flush --skip 5 \
  --ops 10000 --items 200 --refund-pct 20

# 跑到第 50 毫秒无条件终止，模拟随机被杀
./build/rank_crash_helper.exe --dir crash_data --ops 200000 --exit-after-ms 50

# 打印 Open（含恢复）的耗时，D-12 用
./build/rank_crash_helper.exe --dir crash_data --ops 0 --print-open
```

崩溃点：`before_write`（写 `ops.log` 之前）、`after_fsync`（fsync 之后、返回调用方之前）、`during_flush`（刷新中途）、`before_rename`（`rank.dat` 的 `.tmp` 写完还没改名）。终止用 `std::_Exit(42)`，不跑析构、不刷缓冲，退出码固定 42。

崩完再用 `rank_check` 看数据目录，或者直接重启一个引擎比对。
