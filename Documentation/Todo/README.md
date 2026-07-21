# TODO 计划文档指南

本目录保存已经收窄范围、可以直接执行的实施计划。查找或新建 TODO 时，
先读本文档；不需要通读其他 TODO 来推断文档风格。

## 文档索引

| 计划 | 主要范围 |
| --- | --- |
| [SkyBoxComponent](SkyBoxComponent.md) | 第一版静态 Cubemap 天空背景的完整垂直链路 |
| [Texture Support](TextureSupport.md) | Texture2D 资产、平台数据、材质采样与验证 |
| [Material System](MaterialSystem.md) | 材质编辑、表面模型、Shader Map 与运行时材质 |
| [Reflected Property Editing](ReflectedPropertyEditing.md) | 反射属性编辑、事务、通知和定制化 |
| [Multithreading System](MultithreadingSystem.md) | 任务系统、线程边界和渲染并发演进 |
| [Editor Workspace Refactor](EditorWorkspaceRefactor.md) | 编辑器 Workspace 、面板和文档生命周期 |
| [Editor Icon Atlas](EditorIconAtlas.md) | 编辑器可视化图标的离线 Atlas 管线 |

新增、重命名或删除 TODO 文档时，必须同步更新本索引。

## TODO 与其他文档的边界

- `Documentation/Reference` 记录调研、外部案例和候选方案，不代表已选定路线。
- `Documentation/Architecture` 记录已经采用且需要长期维护的架构约束。
- `Documentation/Todo` 记录从当前状态到目标状态的可执行路径和验收门槛。

当选型尚未完成时，先保留在 Reference；只有在范围、非目标和关键技术决策已经
明确后，才形成 TODO。实现落地后，将需要长期遵守的内容迁入 Architecture，
不要让 TODO 成为第二份架构规范。

## 统一文档结构

新 TODO 默认使用以下结构。可以增加专题章节，但不应省略范围、阶段验收和
完成定义。

```markdown
# <Feature> TODO

Last reviewed: YYYY-MM-DD

## Current Status
## Goal
## Scope
## Non-Goals
## Design Decisions and Invariants
## Current Foundations and Gaps
## Implementation Stages
### Stage 0: ...
- [ ] ...
#### Acceptance Gate
- ...
## Validation Matrix
## Definition of Done
## Deferred Follow-ups
## Related Documentation
## Related Code
```

## 编写规则

### 1. 先收窄范围

- `Goal` 用一段话描述用户最终能够看到或使用什么。
- `Scope` 列出必须打通的端到端链路。
- `Non-Goals` 明确排除容易顺手扩张的能力。
- 不使用“完善”、“支持好”、“视情况处理”等无法验收的表述。

### 2. 先写决策和不变量

计划应明确已经选定的输入格式、所有权、线程边界、失败回退和渲染顺序。
如果某项仍需要选型，把它写成 Stage 0 中必须关闭的问题，不要把互相冲突的
候选路线同时写成实施任务。

### 3. 阶段必须可独立验收

每个 Stage 包含：

- 该阶段的产物，而不只是要修改的文件。
- 可勾选的具体任务。
- `Acceptance Gate`，说明什么证据允许进入下一阶段。
- 与前置阶段的显式依赖。

阶段顺序优先按“底层契约 → 资源生命周期 → 场景数据 → 渲染效果 → 编辑器工作流 →
端到端验证”组织，但应以真实依赖关系为准。

### 4. 分开实现任务和验证任务

- 单元测试覆盖数据约束、边界和失败路径。
- 集成测试覆盖资产、反射、序列化、线程与模块边界。
- 渲染功能需要列出真实后端验证和可视结果检查。
- 最终构建、测试和运行方式遵守根目录 `AGENTS.md` 与 Setup 文档，不在每份 TODO 中
  复制一套可能过期的命令。

### 5. 完成后维护状态

- 任务落地时即时勾选，不等到整份计划完成后集中更新。
- 每次实质性更新同步修改 `Last reviewed` 和 `Current Status`。
- 发现实现已经偏离计划时，先更新决策和原因，再继续勾选任务。
- 所有 `Definition of Done` 条件满足后，将长期规则写入 Architecture，然后将 TODO 标记为完成
  或移入历史区；不在本文档里删除其索引而不留下去向。

