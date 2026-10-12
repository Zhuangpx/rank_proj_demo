---
name: dpwf-dev
description: "本 skill 是 dp-workflow 的「开发」阶段，输入需求文档，跑「讨论需求（解歧义）→ 出设计 → 实现 → 跑测试」的 loop，拥有 docs/dp-workflow/design/DESIGN.md（架构设计）、docs/dp-workflow/design/CODE_DESIGN.md（代码级设计）与代码。当用户要求「设计」「实现 / 写代码」「跑测试」「开发某个模块」时触发；发现需求歧义只提 spec_gap 事件回 dpwf-req，不自己改需求。做的是具体设计/代码/测试产物，不是流程导航（问「下一步 / 到哪步」用 dp-workflow）。"
---

# dpwf-dev — 开发 skill

## 职责
输入需求文档 → loop：讨论需求（解歧义）→ 出设计 → 实现 → 跑测试。

## 时机
`current_state ∈ {design, implementation, testing}`。

## 输入
- **调用描述**：设计 / 实现 / 测试某模块的指令。
- **传入文件**：`docs/dp-workflow/requirements/SPEC.md`（需求 + AC）。
- **读取**：`docs/dp-workflow/STATE.md`（确认状态）。

## 输出
- **产出文件**：
  - `docs/dp-workflow/design/DESIGN.md`（架构/技术设计，从 `templates/DESIGN.md` 产出）
  - `docs/dp-workflow/design/CODE_DESIGN.md`（代码级设计，从 `templates/CODE_DESIGN.md` 产出）
  - 代码（带解释性注释）
  - `docs/dp-workflow/testing/TEST_PLAN.md`（从 `templates/TEST_PLAN.md` 产出）
- **事件**：`tests_passed` → `review`；回退 `spec_gap` / `design_gap` / `impl_bug`。

## 模板
`templates/DESIGN.md`、`templates/CODE_DESIGN.md`、`templates/TEST_PLAN.md`（本 skill 自带）。

## 规则
- **代码级设计先行**：实现一个里程碑前，先写 CODE_DESIGN（模块/文件的关系与职责、线程模型、重要业务链路、重要函数与取舍），代码照它写；只捋关键信息，不逐行、不穷举每个函数。
- **双层设计一致 + 文档代码同步**：DESIGN.md（架构级）与 CODE_DESIGN.md（代码级）是同一设计的两个层级，必须一致；代码改动必须同步改 CODE_DESIGN（反之亦然），只改一个不改另一个 = 未完成。
- **实现产出必须带解释性注释**（这是验收标准，不是独立步骤）。
- 讨论需求发现歧义/遗漏 → 提 `spec_gap` 回 `dpwf-req`，**不自己改需求**。
- 测试出 bug → 归类 `impl_bug`，留本 loop 修（不回退）。
- **反向覆盖表是门禁的强制产物**：DESIGN 的「需求覆盖表」、TEST_PLAN 的「验收覆盖表」是 `design_approved` / `tests_passed` 的判定依据，不是可选注释；表里有空行 = 该层未完成。
- **上下文**：见下方「上下文管理」。

## 操作步骤
1. 读 `docs/dp-workflow/STATE.md` 确认状态，读 `docs/dp-workflow/requirements/SPEC.md`。
2. **讨论需求**：发现歧义/遗漏 → 提 `spec_gap` 回 `dpwf-req`，不自己改。
3. **设计**：按 `templates/DESIGN.md` 产出到 `docs/dp-workflow/design/DESIGN.md`（技术选型/架构/数据/接口/里程碑），每项映射回需求条目；**逐条 REQ 反向核对、填「需求覆盖表」，无遗漏（尤其 P0 无「待定 / 一句带过」）**；**每条核心业务流程写「端到端链路」且首尾闭合、无悬空接缝**；**分层图每个模块都有里程碑承接**；三者齐备才 `design_approved`；设计理由写够（不只架构图）。
4. **实现**：先按 `templates/CODE_DESIGN.md` 产出该里程碑的代码级设计到 `docs/dp-workflow/design/CODE_DESIGN.md`（模块/文件关系与职责、线程模型、重要业务链路、重要函数与取舍），再照它写代码（带解释性注释）；代码改动同步更新 CODE_DESIGN，CODE_DESIGN 与 DESIGN 保持一致。
5. **测试**：按 `templates/TEST_PLAN.md` 产出到 `docs/dp-workflow/testing/TEST_PLAN.md`（用例映射 AC），**逐条 AC 反向核对、每条 AC ≥1 用例且用例真测该 AC 声称的行为**，全绿才 `tests_passed`。
6. 每步切换或改动归类后，更新 `docs/dp-workflow/STATE.md` 的 `current_state`。

## 上下文管理

`dpwf-dev` 按 phase 线性推进（`design` → `implementation` → `testing`），`current_state` 全局串行；里程碑是**每个 phase 内部的拆分单元**——一个会话只做一个里程碑的一个 phase，功能之间可分别实现、分别测试：

| phase | 一个会话 = | 最小输入 | 产出 |
|-------|-----------|---------|------|
| `design` | 一个里程碑的**设计** | SPEC | DESIGN 该里程碑段（含设计理由） |
| `implementation` | 一个里程碑的**实现** | SPEC + DESIGN | 该里程碑的 CODE_DESIGN 段 + 代码（带「为什么」注释） |
| `testing` | 一个/多个里程碑的**测试** | SPEC 的 AC + 代码 | TEST_PLAN 该里程碑段（映射 AC） |

例：「两个功能分别实现再分别测试」= `implementation` phase 里 M1、M2 各一个会话；`implementation_complete` 后进 `testing` phase，M1 / M2 再分别（或合并）测。要点不变：DESIGN 写「为什么」、CODE_DESIGN 写「代码怎么组织」、代码注释写「为什么」、测试只对 AC；`impl_bug` 内环留在同一里程碑的实现/测试会话内修。

## 回退分类
- 代码错（spec/design 都对）→ `impl_bug`（留本 loop）
- 设计错 → `design_gap`（回 design）
- 需求错/漏 → `spec_gap`（回 `dpwf-req`，交给 `dpwf-req` 改）
