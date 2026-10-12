---
name: dp-workflow
description: "本 skill 是开发工作流的总编排入口，负责读 STATE.md 判断当前状态、回答「现在到哪一步 / 下一步该做什么」，并把具体动作路由到四个阶段 skill（dpwf-req / dpwf-dev / dpwf-comment / dpwf-review）。当用户要求「跑开发流程」「开始开发」「走一遍 workflow」「现在到哪一步」「当前什么状态」「下一步该做什么」「交接 / 继续」时触发。只做流程/状态/导航，不产出需求、设计、代码等具体产物。"
---

# dp-workflow — 编排入口 skill

## 职责
若 `docs/dp-workflow/STATE.md` 不存在 → 从 `templates/STATE.md` 生成；读 `current_state` → 路由到对应阶段 skill。**只导航，不执行阶段动作。**

## 输入
- **调用描述**：跑开发流程 / 开始开发 / 现在到哪一步 / 下一步该做什么 / 交接 继续。
- **读取**：`docs/dp-workflow/STATE.md`（不存在则先从 `templates/STATE.md` 生成）。

## 输出
- **首次运行**：生成 `docs/dp-workflow/STATE.md`（从 `templates/STATE.md` 复制，`current_state = requirements`）。
- **路由结果**：当前状态 + 下一步该调哪个 `dpwf-*`。
- 之后只读不写 `STATE.md`——`current_state` 由各 `dpwf-*` 在门禁处更新；本 skill 不复制、不另定义状态机。

## 模板
`templates/STATE.md`（本 skill 自带；首次运行时复制到 `docs/dp-workflow/STATE.md`，此后成为运行时唯一事实来源）。

## 路由规则
按 `current_state` 决定下一步：

| current_state | 对应 skill | 该做什么 |
|--------------|-----------|---------|
| `requirements` | `dpwf-req` | 写/补需求（调 `/dpwf-req`） |
| `design` / `implementation` / `testing` | `dpwf-dev` | 设计 / 实现 / 测试（调 `/dpwf-dev`） |
| `review` | `dpwf-comment` ∥ `dpwf-review` | 注释审查 ∥ 代码审查（并行，调 `/dpwf-comment` + `/dpwf-review`） |
| `done` | — | 已验收；新需求走 `new_feature` 回 `requirements` |

## 阶段导航（总览）

| 阶段 | 状态 | skill | 拥有文档 | 进入事件 |
|------|------|-------|---------|---------|
| 需求 | `requirements` | `dpwf-req` | `docs/dp-workflow/requirements/SPEC.md` | — |
| 开发 | `design` `implementation` `testing` | `dpwf-dev` | `docs/dp-workflow/design/DESIGN.md`（架构）+ `docs/dp-workflow/design/CODE_DESIGN.md`（代码级）+ `docs/dp-workflow/testing/TEST_PLAN.md` + 代码 | `requirements_approved` |
| 注释 | `review` | `dpwf-comment` | 代码注释 | `tests_passed` |
| 审查 | `review` | `dpwf-review` | 代码 | `tests_passed` |
| 完成 | `done` | — | — | `review_approved` |

## 操作步骤
1. 若 `docs/dp-workflow/STATE.md` 不存在，从 `templates/STATE.md` 生成（`current_state = requirements`）。
2. 读 `docs/dp-workflow/STATE.md` 的 `current_state`。
3. 按「路由规则」判断下一步该调哪个 `dpwf-*`，输出导航结果。

## 边界
- 本 skill 与 `workflow.html`（可视化）、`templates/STATE.md`（状态机模板）同目录，归 `dp-workflow` 所有；状态机运行时实例 `docs/dp-workflow/STATE.md` 在 `docs/` 下，由模板生成。
- 具体动作（写需求 / 设计 / 实现 / 测试 / 审注释 / 审代码）交 `dpwf-*`，本 skill 不代办。
