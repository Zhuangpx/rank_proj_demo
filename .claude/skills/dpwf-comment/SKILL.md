---
name: dpwf-comment
description: "本 skill 是 dp-workflow 的「注释」阶段，在实现定稿后（tests_passed → review）审查/补全代码注释，只审查不重写代码，与 dpwf-review 并行、不与 dpwf-dev 并行。当用户要求「审注释」「补注释」「检查代码注释」时触发。"
---

# dpwf-comment — 注释 skill

## 职责
实现定稿后，审查/补全注释。**只审查，不重写代码。**

## 时机
门禁之后（`tests_passed` → `review`）；与 `dpwf-review` 并行，**不与 `dpwf-dev` 并行**——注释是消费代码的，不能与生产代码的 `dpwf-dev` 并行。

## 输入
- **调用描述**：审注释 / 补注释 / 检查代码注释。
- **传入文件**：代码 + `docs/dp-workflow/requirements/SPEC.md` + `docs/dp-workflow/design/DESIGN.md`。
- **读取**：`docs/dp-workflow/STATE.md`（确认 `current_state = review`）。

## 输出
- **产出**：注释审查结论（补全/修正注释，不改逻辑）。
- **事件**：如需改代码 → 反馈回 `dpwf-dev`（`review_feedback`）。

## 规则
注释解释「为什么」，不复述「做了什么」。

## 操作步骤
1. 读 `docs/dp-workflow/STATE.md` 确认 `current_state = review`。
2. 读代码 + SPEC / DESIGN，核对注释是否解释「为什么」。
3. 补全缺失/错误的注释，不改逻辑。
4. 产出注释审查结论；如需改代码则反馈回 `dpwf-dev`。
