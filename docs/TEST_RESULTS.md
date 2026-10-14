# 测试结果记录

每做完一轮完整测试记一节，最新的放最上面。一节里要有：版本、环境、自动化用例的结果、半自动用例的**完整命令和数字**、这一轮发现的问题、没验到的东西。命令必须是能原样复跑的，下一个版本靠它跟这一版对比。

用例不过的时候，按 `TEST_PLAN.md` §12 记：用例 ID、完整配置参数、随机种子、`ops.log` 和 `rank.dat`、实际输出与期望输出的差异明细。

## 版本索引

| 版本 | 日期 | 基线提交 | 自动化用例 | 失败 | 备注 |
|---|---|---|---|---|---|
| v1 | 2026-10-14 | `e8f0465`（代码未提交） | 51 | 0 | 阶段 2 首次实现 |

---

# v1 — 2026-10-14

## 环境

| 项 | 值 |
|---|---|
| 系统 | Windows 11 Pro 10.0.26200 |
| CPU | 12th Gen Intel Core i5-12600KF（16 个逻辑线程） |
| 编译器 | MinGW-w64 GCC 15.2.0（x86_64-win32-seh） |
| CMake | 4.3.0-rc2，生成器 `MinGW Makefiles`，`CMAKE_BUILD_TYPE=Release` |
| GoogleTest | **自带的精简版 `tests/gtest_lite`**，机器连不上 googletest 仓库 |
| MSVC | 没装，F-08 只做了 MinGW 这一边 |

构建零警告（`-Wall -Wextra`）。

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 1. 自动化用例

| 跑法 | 命令 | 结果 |
|---|---|---|
| ctest | `ctest --test-dir build --output-on-failure` | 1/1 通过，**22.10 秒** |
| 全量 | `./build/tests/rank_tests.exe` | **51 个用例，0 失败** |
| 全量重复 10 轮 | `./build/tests/rank_tests.exe --gtest_repeat=10` | **510 个用例，0 失败** |
| E-07：并发敏感的 7 条重复 50 轮 | `./build/tests/rank_tests.exe --gtest_repeat=50 --gtest_brief=1 --gtest_filter='A_Submit.A10*:A_Submit.A11*:B_Query.B08*:B_Query.B09*:E_Concurrency.E0[123]*'` | **350 个用例，0 失败** |

覆盖的用例：A-01~A-13、B-01~B-12、C-01~C-06 / C-09 / C-10、D-01~D-11、E-01 / E-02 / E-03 / E-05、F-01~F-06。没自动化的是 C-07、C-08、D-12、E-04、E-06、F-07、F-08，下面逐条记。

## 2. 半自动用例

压测统一关 fsync（`--no-fsync`），固定 `--seed 1`。真 fsync 一次十几毫秒，开着没法在几分钟里跑完一轮对比。

### C-07　合并比随热点集中度变化

```bash
for a in 0 0.5 1.0 1.5; do
  ./build/rank_bench.exe --dir D --threads 8 --ops 1000000 --items 200000 --zipf $a --seed 1 --no-fsync
done
```

| Zipf α | 合并比 | 上榜物品数 | 刷新次数 |
|---|---|---|---|
| 0（均匀） | 0.9686 | 191828 | 93 |
| 0.5 | 0.9343 | 180023 | 90 |
| 1.0 | 0.3881 | 96286 | 38 |
| 1.5 | 0.0325 | 9366 | 15 |

**符合预期**：越集中在少数热门物品，同一个刷新窗口里同一个物品被改的次数越多，合并掉的就越多。α = 1.5 时 100 万次操作只往榜单上应用了 3.25% 的事件。

### C-08　Fresh 读对写入的影响

三组除了标注的参数以外完全一样：

```bash
./build/rank_bench.exe --dir D --threads 8 --ops 100000 --items 200000 --seed 1 --no-fsync --read-ratio 0.5 --fresh-ratio 0
./build/rank_bench.exe --dir D --threads 8 --ops 100000 --items 200000 --seed 1 --no-fsync --read-ratio 0.5 --fresh-ratio 1.0
./build/rank_bench.exe --dir D --threads 8 --ops 100000 --items 200000 --seed 1 --no-fsync --read-ratio 0.5 --fresh-ratio 1.0 --tmin 0
```

| 配置 | 耗时 | 写吞吐 | 提交延迟 P50 | **P99** | P99.9 | 刷新次数 | 单次刷新最长 |
|---|---|---|---|---|---|---|---|
| Fresh 0% | 0.165 s | 303412 ops/s | 0.024 ms | **0.068 ms** | 0.138 ms | 5 | 0.939 ms |
| Fresh 100%（`T_min` 默认 10 ms） | 64.206 s | 780 ops/s | 0.031 ms | **0.138 ms** | 4.920 ms | 6316 | 21.323 ms |
| Fresh 100%，`--tmin 0` | 1.535 s | 32612 ops/s | 0.020 ms | **0.063 ms** | 0.133 ms | — | 0.648 ms |

**结论要分两层看**：

- 写吞吐掉了 389 倍，但这**不是** FR-11 不成立。压测的工作线程读写共用，一个线程发了 `Fresh` 读就要卡在最小刷新间隔上等，它这一轮的写也跟着停。把 `T_min` 设成 0 再跑，同样 100% `Fresh` 读下写吞吐回到 32612 ops/s。**计划里「两组写性能不应有数量级差距」这个判据选错了**，已经改成看提交延迟 P99。
- 按提交延迟 P99 看：0.068 → 0.138 ms，2 倍，同一个数量级，FR-11 成立。
- **但 P99.9 从 0.138 ms 涨到 4.920 ms，36 倍，这一条要记下来**。原因是刷新次数从 5 次涨到 6316 次，刷新线程几乎一直在干活，单次刷新最长 21.3 ms，和提交路径抢 CPU。这不影响正确性，但「`Fresh` 读的开销完全不落在提交线程上」这句话在最长尾的那 0.1% 上并不成立。

### D-12　启动耗时

用压测程序造数据，再用崩溃注入程序量 `Open` 的耗时（恢复就在 `Open` 里做完）：

```bash
./build/rank_bench.exe --dir D --threads 8 --ops N --items 200000 --seed 1 --no-fsync --refund-ratio 0.2
./build/rank_crash_helper.exe --dir D --ops 0 --print-open     # 留着 rank.dat
rm D/rank.dat
./build/rank_crash_helper.exe --dir D --ops 0 --print-open     # 删掉 rank.dat
```

| `ops.log` 记录数 | 留 `rank.dat` | 删 `rank.dat` | 省下 |
|---|---|---|---|
| 96068 | 21.642 ms | 28.600 ms | 24% |
| 960486 | 233.518 ms | 287.383 ms | 19% |
| 4800513 | 1587.512 ms | 2004.476 ms | 21% |

**不符合计划里的预期**。计划写的是「保留 `rank.dat` 应明显更快，差距随文件变长而拉大」，实测是**固定省两成左右，不随文件变长拉开**。原因：订单表没有对应的存档，恢复时每条记录都要无条件重放一遍去重建订单表（`src/recovery.cpp:67`），`rank.dat` 省掉的只是把 delta 累加到计数表那一下。这是当前设计的既定行为，已记进 `TEST_PLAN.md`。

连带的范围限制：`ops.log` 永远不压缩，启动耗时是 `O(全部记录数)`，480 万条 1.6 秒，1 亿条大约 33 秒。demo 够用，长期跑要加日志压缩。

### E-04　64 线程长时间压测

```bash
./build/rank_bench.exe --dir D --threads 64 --ops 2000000 --items 200000 --seed 1 --no-fsync --refund-ratio 0.25 --read-ratio 0.2
./build/rank_check.exe D
```

| 项 | 值 |
|---|---|
| 耗时 | 13.121 s |
| 写操作 | 1599978（成功购买 1200341，成功回退 299731） |
| 写吞吐 / 读吞吐 | 121938 / 30487 ops/s |
| 提交延迟 P50 / P99 / P99.9 | 0.039 / 1.846 / 2.171 ms |
| 刷新次数 | 137，平均 4.334 ms，最长 6.421 ms |
| 合并比 | 0.9664 |
| delta 提交 / 应用 | 900610 / 900610 **一致** |
| 上榜物品数 | 197850 |

对账（`rank_check`）：`ops.log` 1500072 条，长度是 32 的整数倍，ts 严格递增；`rank.dat` 长度 3192224 = 16 + 16 × 199513，有效；`ts > ts_merged` 的记录 0 条；`rank.dat` 的计数等于 `ops.log` 中 `ts ≤ ts_merged` 的 delta 之和。**全部一致。**

### F-07　压测程序的开关

固定 `--ops 200000 --items 5000 --seed 1 --no-fsync`，每次只动一个：

**`--threads`**（这一组测了两轮，结果稳定）

| 线程数 | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 写吞吐 ops/s | 52084 | 140507 | 257372 | **343606** | 219424 |

8 个线程到顶，16 个反而回落。这台机器 16 个逻辑线程，压测要起 16 个提交线程加日志线程加刷新线程，一共 18 个要跑的线程，排不开。**开关本身是生效的**，这个回落是机器的事。

**其余四个**

| 开关 | 取值 | 结果 |
|---|---|---|
| `--ops` | 50000 → 200000 | 写操作 50000 → 200000，`ops.log` 记录数 48081 → 192098，正好 4 倍 |
| `--refund-ratio` | 0 → 0.5 | 成功回退 0 → 49999 |
| `--items` | 100 → 20000 | 上榜物品数 100 → 19968，合并比 0.0016 → 0.7034 |
| `--zipf` / `--read-ratio` | 见 C-07 / C-08 | 生效 |

六个开关全部验过，都生效。

### F-08　两种编译器各构建一次

**只做了一半。** 这台机器上 `cl.exe`、`vswhere.exe`、Visual Studio 目录都没有，MSVC 那边**没有实测过**。MinGW 这边 `-Wall -Wextra` 零警告。

CMake 里 MSVC 的开关已经配好（`/utf-8 /W4 /permissive-`）。源码里唯一的平台分支在 `src/util.cpp` 的 `SyncFile()`：Windows 走 `<io.h>` 的 `_commit`，其他平台走 `<unistd.h>` 的 `fsync`——标准库里没有把文件刷到盘上的接口，这一处躲不掉。

### E-06　写失败后的行为

**没做。** 要人为制造磁盘写失败，没自动化也没手工跑。相关的问题见下面第 4 节。

## 3. 这一轮修掉的缺陷

| # | 用例 | 问题 | 修法 |
|---|---|---|---|
| 1 | C-06 | `Engine::Open` 发第一份快照时直接用了 `rr.ts_merged`，空目录启动时它是 0，读请求算出来的陈旧度是 `1791959466802681000` ns——从 1970 年算起的那个数 | 日志先开，从 `NextTs()` 取一个戳给初始快照。此时日志线程还没启动，这个戳一定大于所有历史记录、小于之后每一条，恢复判据不变（`src/engine.cpp`） |
| 2 | 150 万条压测的手工对账 | `rank_check` 把「`ts_merged` 不晚于末条记录 `ts`」当判据，而干净退出时最后一次刷新本来就发生在最后一条记录之后，正常状态被报成异常 | 换成真正的不变量：`rank.dat` 的计数 == `ops.log` 中 `ts ≤ ts_merged` 那部分 delta 之和，并打印差异明细（`tools/rank_check.cpp`、`tools/ops_scan.h`） |
| 3 | 第一次跑用例时全部卡死 | Git 自带的 `libstdc++-6.dll`（posix 线程版）被 `PATH` 先找到，和 exe 里内联的 win32 线程版对不上，表现是信号量 `release` 了但 `acquire` 不醒 | `CMakeLists.txt` 加 `-static -static-libgcc -static-libstdc++` |
| 4 | 压测 | Windows 的定时等待被放大到 15.6 ms，日志线程靠定时轮询时写吞吐只有 **536 ops/s** | 切表请求也算「有活干」去唤醒日志线程；合批窗口默认改成 0，靠写盘本身的耗时自然攒批。改完 **187573 ops/s** |
| 5 | B-09、C-03 | 用例自身设计有问题，不是引擎的问题：2000 次 `Stale` 读几微秒就跑完，一次刷新都没发生；单线程串行 `Fresh` 读时读次数天生等于刷新次数 | 改成按时间读 / 8 个读线程，`TEST_PLAN.md` 对应行补上了原因 |

## 4. 看代码发现、这一轮没实测到的问题

**写失败后 `ts_merged` 会越过没合并的记录**（`src/op_log.cpp:150-170`）：`fwrite` 成功但 `fsync` 失败时，这一批不投递到合并表、引擎转只读，但字节已经写进 `ops.log` 了。刷新线程不受只读状态影响，会继续推进 `ts_merged` 并把 `rank.dat` 存下去。重启后恢复按 `ts > ts_merged` 补算，这一批被永远跳过——而订单表是无条件重放的，于是 INV-1（榜单 == 订单表求和）不成立，计数永久少算。

对应用例 E-06，这一版没跑，所以**没有实测证据**。最省事的修法是写失败之后让刷新线程也停下来、不再更新 `rank.dat`。

## 5. 文档和实现对不上的地方

| 处 | 文档写的 | 实现是 |
|---|---|---|
| FR-14 | 每条记录 `ts / op_type / order_id / uid / item_id / qty` | `(ts, order_id, item_id, delta)` 共 32 字节；`op_type` 用 `delta` 的符号顶替，`uid` 接口收下但不落盘 |
| FR-7 | 返回前 k 名和快照版本号 | 还返回了 `ts_merged`，C-06 就靠它测陈旧度 |
| FR-12 | 「取走的恰好是 `ts ≤ ts_merged` 的全部操作」 | 正常路径上成立；第 4 节那个写失败的洞让它在异常路径上不成立 |

三处都还没定怎么改，等确认。
