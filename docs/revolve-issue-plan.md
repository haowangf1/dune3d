# Revolve 交互式选轴方案

## Issue 原文
> It's a bit confusing, that one has to select an edge/workplane before adding a revolve group.
> It would be nice to have a small interactive "revolve tool" similar to the "draw contour" tool or the "select edges" tool. that starts when adding a revolve group.

## Issue 翻译
> 目前在添加 `revolve group` 之前，必须先选中一条边或一个工作平面，这一点有点让人困惑。
> 如果在添加 `revolve group` 时，能像 `draw contour` 工具或 `select edges` 工具那样，启动一个小型交互式的“revolve tool”，会更好。

## 背景
当前创建 `Revolve Group` 时，用户必须先预选一条轴线或工作平面，再执行“添加 revolve group”。

这会带来两个问题：
- 前置条件不明显，容易困惑
- 交互流程不如 `draw contour`、`select edges` 这类工具自然

issue 的目标不是修改 revolve 几何算法，而是优化创建流程。

## 目标
将当前流程从：

- 先选 axis
- 再添加 revolve group

改为：

- 先添加 revolve group
- 自动进入一个小型交互工具
- 在工具中选择 axis
- 选择完成后提交

## 现状
当前 `revolve` 的创建逻辑在：
- `src/editor/editor_workspace_browser.cpp`

现状特点：
- 依赖当前 selection
- 在 `on_add_group()` 中直接校验 axis
- 没有独立的交互 tool

参考模式：
- `FILLET` / `CHAMFER` 创建后自动触发 `SELECT_EDGES`
- `PIPE` 创建后自动触发 `SELECT_SPINE_ENTITIES`

这说明“先建 group，再进入 tool”是项目中已有模式。

## 推荐实现
采用最小改动方案：

### 1. 新增一个专用 tool
新增：
- `ToolID::SELECT_REVOLVE_AXIS`
- `src/core/tools/tool_select_revolve_axis.hpp`
- `src/core/tools/tool_select_revolve_axis.cpp`

职责：
- 只负责为当前 `GroupRevolve` 选择 axis
- 选中后写回 group
- 完成后提交

### 2. 修改 revolve group 创建流程
在 `Editor::on_add_group()` 中：
- 保留“当前 group 必须有 active workplane”的检查
- 创建 `GroupRevolve`
- 设置：
  - `m_wrkpl`
  - `m_source_group`
- 不再要求创建前必须已有 selection

### 3. 创建后自动进入选轴工具
在 `Editor::finish_add_group()` 中，对 `Group::Type::REVOLVE` 增加：
- `trigger_action(ToolID::SELECT_REVOLVE_AXIS)`

效果：
- 用户点击“添加 revolve group”后，立即进入选轴模式

## 新 tool 的行为
建议做成最简单版本。

### can_begin()
只允许在当前 group 为 `GroupRevolve` 时启动。

### begin()
进入工具后：
- 开启 hover selection
- 设置工具栏提示
- 提示用户选择 axis

建议动作：
- `LMB`：选择 axis
- `CANCEL`：取消

如果你想和现有工具保持一致，也可以加：
- `RMB`：结束

但对这个场景来说，**左键选中后直接提交** 会更简单。

### update()
#### 悬停时
只对以下实体视为合法目标：
- `WORKPLANE`
- `LINE_2D`
- `LINE_3D`

并且要求：
- 选中的是 entity body，即 `point == 0`

#### 左键时
如果 hover 到合法 axis：
- 写入 `group.m_origin = {axis_entity, 1}`
- 写入 `group.m_normal = axis_entity`
- 标记当前 group 需要重新生成
- `commit`

如果不合法：
- 不提交
- 给出简短提示或忽略点击

#### 取消时
- `revert`

## 需要复用的现有校验逻辑
不要重新设计规则，直接复用当前 `on_add_group()` 里对 axis 的要求：
- 必须选择 entity body
- 实体类型必须是 line 或 workplane

这样改动最小，也最符合现有行为。

## 建议改动文件
必改：
- `src/editor/editor_workspace_browser.cpp`
- `src/core/tool_id.hpp`
- `src/core/create_tool.cpp`
- `src/core/tools/tool_select_revolve_axis.hpp`
- `src/core/tools/tool_select_revolve_axis.cpp`

可能需要：
- tool 注册相关文件
- action / in-tool action 相关注册文件

## 不建议做的事
这次先不要做这些：
- 不要改 `GroupRevolve` 的几何生成逻辑
- 不要顺手抽象成通用 circular sweep 选轴系统
- 不要大改 `lathe` 流程
- 不要加入额外复杂 UI

先把 issue 需要的最小交互流程做出来。

## 最小验收标准
完成后至少确认：

1. 当前 group 没有 active workplane 时，仍然不能创建 revolve
2. 创建 revolve 时不再要求用户预选 axis
3. 创建后会自动进入选轴模式
4. 选择 workplane body 可以成功
5. 选择 line body 可以成功
6. 点击 point 或非法实体不会错误提交
7. 取消后不会留下错误状态
8. 创建成功后 group editor 和后续 revolve 行为正常

## 一句话总结
这次改动的核心是：

**把 `Revolve Group` 从“依赖预选 axis 的命令”改成“创建后自动进入选轴的小型交互工具”。**



步骤
src/core/tool_id.hpp
      - 加 ToolID::SELECT_REVOLVE_AXIS
  - src/core/tools/tool_select_revolve_axis.hpp
  - src/core/tools/tool_select_revolve_axis.cpp
      - 新工具本体：can_begin/begin/update
      - 逻辑：只接受 WORKPLANE/LINE_2D/LINE_3D 且 point==0
  - src/core/create_tool.cpp
      - include 新 tool
      - 在 switch 里注册 ToolID::SELECT_REVOLVE_AXIS -> ToolSelectRevolveAxis
  - meson.build
      - 把 src/core/tools/tool_select_revolve_axis.cpp 加到 src_gui 文件列表
  - src/action/action_catalog.cpp
      - 加 ToolID::SELECT_REVOLVE_AXIS 的 action catalog（建议 hidden）
      - 在 tool_lut 里加映射项
  - src/editor/editor_workspace_browser.cpp
      - on_add_group()：把 REVOLVE 从“先选轴”逻辑里拆出来（创建时不再要求 selection）
      - finish_add_group()：Group::Type::REVOLVE 后 trigger_action(ToolID::SELECT_REVOLVE_AXIS)