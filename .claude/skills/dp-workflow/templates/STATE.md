# STATE — dp-workflow 开发工作流状态机

> `dp-workflow` · 控制流层 · owner 全局

> 本文件由 `dp-workflow` 首次运行时从 `.claude/skills/dp-workflow/templates/STATE.md` 生成（项目开始前 `docs/dp-workflow/` 为空）。生成后即成为控制流唯一事实来源：入口 skill `dp-workflow` 读本文件路由到四个阶段 skill（`dpwf-*`），各拥有若干状态；遇到改动先归类事件，再查迁移表跳转。可视化见 `.claude/skills/dp-workflow/workflow.html`。
> 每次会话开始先读本文件；每次阶段切换或改动归类后，更新「当前状态」这一行。多窗口协作见下文「会话交接」。

---

## 当前状态

```text
current_state = requirements
```

---

## 四个阶段 skill 与状态归属

> 入口：`dp-workflow` 读「当前状态」后路由到对应 `dpwf-*`；它不拥有状态，只做导航。

| skill | 拥有状态 | 职责 |
|-------|---------|------|
| `dpwf-req`（需求）     | `requirements` | 产出/版本化更新需求文档（**唯一 owner**） |
| `dpwf-dev`（开发）     | `design` `implementation` `testing` | 讨论需求 → 设计 → 实现 → 测试（loop） |
| `dpwf-comment`（注释） | `review` 注释部分 | 实现定稿后，审查/补全注释 |
| `dpwf-review`（审查）  | `review` 代码部分 | 实现定稿后，对照需求 + 设计审查 |

---

## 状态定义

| 状态 | 含义 | 完成标志 |
|------|------|---------|
| `requirements`   | 需求定义   | 需求文档定稿，每条 AC 可观测、可验证 |
| `design`         | 设计       | 设计定稿，需求覆盖表逐条 REQ 有落点 + 分层图每模块有里程碑承接 + 核心流程端到端链路闭合 |
| `implementation` | 实现       | 代码写完，**带解释性注释**，代码级设计（CODE_DESIGN）到位 |
| `testing`        | 测试       | 逐条 AC 有对应用例且全绿 |
| `review`         | 审查       | 注释审查 + code review 通过 |
| `done`           | 完成       | 需求覆盖表四层闭环 + 逐条 AC 通过 |

---

## 门禁（向前迁移）

| 事件 | 从 → 到 |
|------|---------|
| `requirements_approved`   | requirements → design |
| `design_approved`         | design → implementation |
| `implementation_complete` | implementation → testing |
| `tests_passed`            | testing → review |
| `review_approved`         | review → done |

---

## 回退事件（分级回退）

| 事件 | 含义 | 回退到 |
|------|------|--------|
| `spec_gap`        | 需求歧义/遗漏/错误（dpwf-dev 讨论时发现） | requirements |
| `new_feature`     | 新增需求（追加） | requirements |
| `design_gap`      | 设计错误 | design |
| `impl_bug`        | 测试发现的 bug（代码没按 spec/design 写对） | implementation |
| `review_feedback` | review 意见（视类型） | implementation / design / requirements |

**归类决策树：**

```text
1. 加新东西 or 修已有的？  加 → new_feature ；修 → 往下
2. 问题出在哪一层？
     代码错（spec/design 都对） → impl_bug
     设计错（spec 对）          → design_gap
     需求/验收标准错或漏         → spec_gap
3. review 提的意见，按意见落在哪一层归类。
```

---

## 迁移表（当前状态 × 事件 → 下一状态）

| 当前状态 | 事件 | 下一状态 | 动作 |
|---------|------|---------|------|
| `requirements`   | `requirements_approved` | `design` | 需求定稿，进设计 |
| `requirements`   | `new_feature`           | `requirements` | 追加条目，版本 +1 |
| `design`         | `design_approved`       | `implementation` | 设计定稿，进实现 |
| `design`         | `design_gap`            | `design` | 修订设计 |
| `design`         | `spec_gap`              | `requirements` | 回退补需求 |
| `implementation` | `implementation_complete`| `testing` | 实现完成（带注释），进测试 |
| `implementation` | `impl_bug`              | `implementation` | 内环修代码 |
| `implementation` | `design_gap`            | `design` | 回退改设计 |
| `implementation` | `spec_gap`              | `requirements` | 回退补需求 |
| `testing`        | `tests_passed`          | `review` | 测试全绿，进审查 |
| `testing`        | `impl_bug`              | `implementation` | 内环修代码 |
| `testing`        | `design_gap`            | `design` | 回退改设计 |
| `testing`        | `spec_gap`              | `requirements` | 回退补需求 |
| `review`         | `review_approved`       | `done` | 审查通过 |
| `review`         | `review_feedback`       | 视意见 → implementation / design / requirements | 打回重改 |
| `done`           | `new_feature`           | `requirements` | 新一轮迭代 |

---

## 会话交接（多窗口协作）

> 每个 skill 可跑在独立的 Claude 窗口，上下文不共享，靠文档交接。**未交接 = 未完成**：窗口结束但没更新状态 / 文档，状态机就停在半路。

### 窗口启动（读盘）

1. 读本文件 → 看 `current_state`，判断本窗口该跑哪个 skill。
2. 读上一环节拥有的文档（`docs/dp-workflow/requirements/SPEC.md` / `docs/dp-workflow/design/DESIGN.md` / `docs/dp-workflow/design/CODE_DESIGN.md` / 代码 / `docs/dp-workflow/testing/TEST_PLAN.md`）。
3. 按 `.claude/skills/` 里该 skill 的定义执行。

### 窗口结束（落盘，才叫交接）

1. 更新自己拥有的文档，并在「变更记录」追加一行（版本 +1）。
2. 更新本文件的 `current_state` 那一行。
3. **只在门禁事件发生时交接**：`requirements_approved` / `design_approved` / `tests_passed` / `review_approved`。其余（如内环 `impl_bug` 修代码）留在同一窗口，不交接。

### 能拆 / 不能拆

| 拆法 | 安全 | 说明 |
|------|------|------|
| skill 之间（门禁处） | ✅ 安全 | 文档边界清晰，读上一份文档即可续 |
| dpwf-dev 内部「设计 → 实现」 | ⚠️ 谨慎 | 实现依赖"为什么这么设计"；要拆必须让 `docs/dp-workflow/design/DESIGN.md` 写够设计理由（不只是架构图 + 接口）、`CODE_DESIGN.md` 写够代码结构（目录/文件/类/函数） |
| dpwf-comment / dpwf-review | ✅ 天然并行 | 两个独立窗口，各读代码 + 文档，产出各自反馈 |

---

## 五条修正规则（相对原始草案的关键改动）

1. **并行只在实现定稿后**：`dpwf-comment` 与 `dpwf-review` 互相并行，但都**不**与 `dpwf-dev` 并行。开发生产代码，注释/审查消费代码，消费者不能与生产者并行。
2. **注释是实现的验收标准**：`dpwf-dev` 的「实现」产出必须带解释性注释；`dpwf-comment` skill 只做审查/查漏，不重写代码。
3. **需求单一 owner**：需求文档归 `dpwf-req` 所有（版本化）。`dpwf-dev` 不直接改需求，只提 `spec_gap` / `new_feature` 事件回 `dpwf-req`。
4. **测试逐条反向覆盖验收标准**：`testing` 的用例必须**逐条**映射回需求 AC（每条 AC ≥1 用例），且用例真测该 AC 声称的行为（不能由不相关替身顶替），否则「测试出 bug → loop」无据可依。
5. **上下文按里程碑拆分**：`dpwf-dev` 按 phase 线性推进（`design` → `implementation` → `testing`），每个 phase 内按里程碑/模块拆会话——一个会话只做**一个里程碑的一个 phase**；实现阶段先出 `CODE_DESIGN`（代码级设计）再写代码。跨会话靠交接文档衔接，避免上下文膨胀。

## 终止条件

需求覆盖表逐行闭环 + 核心流程端到端链路闭合 + `review_approved`，或轮次 / token 预算耗尽。

---

## 变更日志

> 每次状态切换 / 改动归类后，追加一行（版本 +1），不覆盖历史。

```text
<生成日期>  init  初始化状态机（dp-workflow 首次运行生成；current_state = requirements）
```
