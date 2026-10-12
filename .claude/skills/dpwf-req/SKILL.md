---
name: dpwf-req
description: "本 skill 是 dp-workflow 的「需求」阶段，把模糊需求落地成两份版本化文档：项目总览 docs/dp-workflow/requirements/OVERVIEW.md（介绍 + 功能等基本信息）与需求规格 docs/dp-workflow/requirements/SPEC.md（每条需求带可验证的 AC）。当用户要求「写需求」「定需求」「补充需求」「改验收标准」「写项目总览 / 项目介绍」「把需求整理成 SPEC」时触发；也用于接收 dpwf-dev 提回的 spec_gap / new_feature 事件。本 skill 是 docs/dp-workflow/requirements/ 下这两份文档的唯一 owner；只做需求文档本身，不做流程导航（问「下一步 / 到哪步」用 dp-workflow）。"
---

# dpwf-req — 需求 skill

## 职责
输入模糊需求 → 产出/更新两份文档：项目总览（介绍 + 功能总览）＋ 需求规格（REQ + AC）；支持动态更新（先定大框架，后续按事件补充）。

## 时机
`current_state = requirements`；或收到 `dpwf-dev` 提回的 `spec_gap` / `new_feature` 事件。

## 输入
- **调用描述**：模糊或明确的需求（要做什么、给谁用、解决什么问题）。
- **传入文件**：无（首次）；或 `spec_gap` / `new_feature` 事件的补充说明。
- **读取**：`docs/dp-workflow/STATE.md`（确认 `current_state = requirements`）。

## 输出
- **产出文件**：
  - `docs/dp-workflow/requirements/OVERVIEW.md`（从 `templates/OVERVIEW.md` 产出，项目总览：介绍 + 功能等基本信息）
  - `docs/dp-workflow/requirements/SPEC.md`（从 `templates/SPEC.md` 产出，REQ + AC，版本 +1）
- **事件**：`requirements_approved` → `design`。

## 模板
`templates/OVERVIEW.md`、`templates/SPEC.md`（本 skill 自带；产出时复制并填充 `<…>` 占位）。

## 边界
只有 `dpwf-req` 能改需求层文档（OVERVIEW + SPEC）。`dpwf-dev` 发现歧义/遗漏时只提事件回来，不自己改——需求单一 owner。

## 操作步骤
1. 读 `docs/dp-workflow/STATE.md` 确认 `current_state = requirements`。
2. 按 `templates/OVERVIEW.md` 产出 `docs/dp-workflow/requirements/OVERVIEW.md`：项目简介 → 核心功能（映射 REQ）→ 架构分层 → 使用方式 → 范围边界。**架构分层的每一层、使用方式的形态，都要在 SPEC 有对应 REQ（含 AC）**——横切交付层（CLI / MCP server / 平台适配层）同样是需求，不能只留在 OVERVIEW 散文。
3. 按 `templates/SPEC.md` 产出 `docs/dp-workflow/requirements/SPEC.md`：项目概述 → 目标/非目标 → 需求列表（REQ 编号 + AC）→ 非功能需求。**把 OVERVIEW 的架构分层 / 交付形态逐条落成带 AC 的 REQ**，不遗漏横切层。
4. 每条需求带可验证的 AC，测试直接对着 AC 写——AC 是「测试派生」的唯一依据。
5. 每次改动在「变更记录」追加一行，版本 +1，不覆盖历史。
6. 定稿后更新 `docs/dp-workflow/STATE.md` 的 `current_state = design`（触发 `requirements_approved` 门禁），完成交接。
