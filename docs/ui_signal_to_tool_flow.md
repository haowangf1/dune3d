# UI 信号 -> Editor -> Tool 调用链（精简版）

本文记录本项目中交互事件的主干路径：**控件发信号，Editor 统一编排，Core/Tool 执行业务**。

## 核心分层

- `widgets / workspace_browser / canvas`：负责 UI 与原始事件（点击、按键、悬停）。
- `editor/*`：控制器层，负责连接信号、路由动作、维护界面状态。
- `core/* + core/tools/*`：工具层，处理交互状态机与模型改动。
- `document/*`：数据模型层（groups/entities/constraints）与重建。

## 关键机制（要害）

- 控件不直接改建模数据，先发 GTK/sigc++ 信号。
- `Editor` 在初始化时统一 `connect(...)`，把信号映射到 `on_xxx` 或 `trigger_action(...)`。
- Tool 激活后，`Editor` 将鼠标/键盘转成 `ToolArgs`（如 `MOVE`、`ACTION:LMB`、`ACTION:CANCEL`），交给 `m_core.tool_update(...)`。
- Tool 返回 `ToolResponse`（`commit/revert/nop`），`Editor::tool_process(...)` 负责刷新画布与 UI。

## 一条完整链路示例（Add Group -> Revolve）

1. **UI 菜单触发**
   - `src/editor/workspace_browser.cpp`
   - `actions->add_action("revolve", [this] { emit_add_group(Group::Type::REVOLVE); });`

2. **Editor 接信号**
   - `src/editor/editor_workspace_browser.cpp`
   - `signal_add_group().connect(..., &Editor::on_add_group)`

3. **Editor 执行业务入口**
   - `Editor::on_add_group(...)` 创建对应 group
   - `Editor::finish_add_group(...)` 做 rebuild / 选中新组
   - 可在此 `trigger_action(ToolID::XXX)` 自动进入工具

4. **Tool 启动与事件分发**
   - `src/editor/editor_tool.cpp`：`tool_begin(...)` -> `m_core.tool_begin(...)`
   - `src/editor/editor.cpp`：鼠标/键盘事件转 `ToolArgs` 后 `m_core.tool_update(...)`

5. **Tool 处理并提交**
   - `src/core/tools/tool_*.cpp` 的 `begin()/update()`
   - 返回 `ToolResponse::commit()` 后由 Editor 统一刷新与收尾

> 结论：该项目采用“**信号驱动 + Editor 编排 + Tool 状态机**”模式，适合做可组合的交互工具（如 Select Edges / Revolve Axis）。
