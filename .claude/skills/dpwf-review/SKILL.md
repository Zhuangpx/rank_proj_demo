---
name: dpwf-review
description: "本 skill 是 dp-workflow 的「审查」阶段，在实现定稿后对照需求 + 设计审查代码，产出 review_feedback 事件（回 dpwf-dev 修，或视类型回 design / requirements）。与 dpwf-comment 并行、不与 dpwf-dev 并行。当用户要求「审查代码」「code review」「review 一下」时触发。"
---

# dpwf-review — 审查 skill

## 职责
实现定稿后，对照需求 + 设计审查代码。

## 时机
门禁之后（`tests_passed` → `review`）；与 `dpwf-comment` 并行。

## 输入
- **调用描述**：审查代码 / code review / review 一下。
- **传入文件**：代码 + `docs/dp-workflow/requirements/SPEC.md` + `docs/dp-workflow/design/DESIGN.md`。
- **读取**：`docs/dp-workflow/STATE.md`（确认 `current_state = review`）。

## 输出
- **产出**：`review_feedback` 事件（意见按落点归类）。
- **事件**：通过 → `review_approved`（→ `done`）；否则 `review_feedback`（→ `implementation` / `design` / `requirements`）。

## 规则
意见按落点归类：代码问题 → `implementation`；设计问题 → `design`；需求问题 → `requirements`。
- **四层闭环核查**：对照 SPEC **逐条 REQ** 核对「需求→设计→实现→测试」闭环——每条 REQ 在 DESIGN 有落点、在代码有实现、在 TEST_PLAN 有用例。缺环按落点归 `review_feedback`：缺设计→`design`、缺实现→`implementation`、缺测试→`testing`、AC 本身缺→`requirements`。
- **链路闭合核查**：对照 DESIGN 的「端到端业务流程链路」逐条核对——每条链路首尾闭合、无悬空接缝，且链路里每个环节在代码里都有真实实现（不是桩 / `NotSupportedError`）；截断按落点归 `review_feedback`。

## 操作步骤
1. 读 `docs/dp-workflow/STATE.md` 确认 `current_state = review`。
2. 对照 `docs/dp-workflow/requirements/SPEC.md`（需求 + AC）和 `docs/dp-workflow/design/DESIGN.md`（设计）审查代码。
3. 意见按落点归类。
4. 通过 → 更新 `docs/dp-workflow/STATE.md` 的 `current_state = done`（`review_approved`）；否则产出 `review_feedback` 回退。
