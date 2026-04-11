# Dune3D UI 架构（精简要害）

本文整合 `ui_signal_to_tool_flow.md` 与现有说明，只保留最关键链路：
- 分层职责
- UI 结构
- 信号到 Tool 的调用流
- pending 队列与防重入机制
- 渲染主链路

---

## 1) 分层与职责

```text
main
  -> Dune3DApplication (应用生命周期)
      -> Dune3DAppWindow (窗口壳 + window.ui)
          -> Editor (事件编排/动作分发/UI联动)
              -> Core (文档/历史/工具状态机)
              -> Tool (begin/update/end)
              -> Renderer (文档 -> 绘制命令)
              -> Canvas (GL视口 + 输入 + 渲染)
```

要点：
- GTK 负责事件循环和控件回调。
- `Editor/Core/Tool` 的状态编排、队列、提交回滚是项目自实现。

---

## 2) Window 结构（window.ui + 运行时注入）

```text
GtkApplicationWindow
├─ HeaderBar
└─ 主体
   └─ Paned
      ├─ left_bar (上下)
      │  ├─ WorkspaceBrowser
      │  └─ PropertiesNotebook(Group/Constraints/Selection)
      └─ 右侧主区
         ├─ version_info_bar
         ├─ Overlay
         │  ├─ Canvas (运行时插入到 canvas_box)
         │  ├─ AxesCube
         │  ├─ welcome_box / action_bar / tool_bar / delete_revealer
         └─ workspace_notebook
```

要点：
- `Canvas` 是项目自定义类，继承 `Gtk::GLArea`，不是 GTK 原生业务控件。

---

## 3) UI 信号 -> Editor -> Tool（核心链路）

### 3.1 鼠标移动为何会进 `handle_cursor_move`

```text
GTK motion event
  -> Canvas::EventControllerMotion 回调
  -> Canvas 发射 signal_cursor_moved
  -> Editor::handle_cursor_move
  -> m_core.tool_update(MOVE)
  -> m_tool->update(args)
```

### 3.2 `tool_update` 在何时调用

`tool_update` 不是轮询；是 Editor 在交互事件中主动调用。高频入口：
- MOVE（鼠标移动）
- ACTION:LMB / ACTION:RMB
- ACTION:CANCEL（例如 ESC）
- VIEW_CHANGED（工具声明需要时）
- DATA（对话框/面板回传）
- pending 队列消费阶段

一句话：**工具激活期间，输入事件会被 Editor 封装成 `ToolArgs` 后交给 `m_core.tool_update(args)`。**

### 3.3 为什么看到“循环调用 update”

```text
Editor::tool_process()
  tool_process_one()
  while (auto args = m_core.get_pending_tool_args()) {
      m_core.tool_update(*args);
      tool_process_one();
  }
```

这不是死循环，而是“清空本轮积压事件”的事件泵。

---

## 4) pending 队列（防重入关键）

- 队列：`Core::m_pending_tool_args`（`std::list<ToolArgs>`）
- 入队时机：`Core::tool_update()` 发现 `m_tool_state != NONE`
  - 说明当前正处于 begin/update，发生重入
  - 新事件先 `emplace_back(std::move(args))`
- 出队时机：`Editor::tool_process()` 中 `get_pending_tool_args()` 逐个 `front/pop_front`

一句话：**这是项目自己的防重入队列，不是 GTK 自带队列。**

---

## 5) 示例链路：Add Group -> Revolve

```text
WorkspaceBrowser 菜单动作
  -> 发射 signal_add_group(Group::Type::REVOLVE)
  -> Editor::on_add_group(...)
  -> 创建 group + finish_add_group/rebuild
  -> (可选) trigger_action(ToolID::...)
  -> tool_begin/tool_update
  -> Tool::begin/update
  -> ToolResponse(commit/revert/end)
  -> Editor::tool_process 刷新 UI/Canvas
```

用途：快速定位“点了某个 UI 操作后，为什么会进入某个 Tool”。

---

## 6) 渲染主链路

```text
Core/Document 状态
  -> Editor::canvas_update()
  -> Renderer::render(...)
  -> Canvas::request_push()
  -> Canvas::on_render()/render_all()
  -> Face/Line/Glyph/Icon/Picture renderer OpenGL 绘制
```

---

## 7) 常见排查顺序

1. 事件是否 connect 到 Editor（`init_*`）
2. Editor 是否触发 `tool_begin/tool_update`
3. Core 是否入队（`m_tool_state != NONE`）
4. `tool_process` 是否消费了 pending
5. `canvas_update -> request_push -> on_render` 是否走通
