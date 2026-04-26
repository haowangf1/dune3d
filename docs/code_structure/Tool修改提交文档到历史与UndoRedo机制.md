# Tool修改提交文档到历史与UndoRedo机制

这篇文档回答 4 个问题：

- tool 是什么时候真正修改 `Document` 的
- 为什么拖动时模型会实时变化，但不会立刻产生很多条 undo
- 什么时机会把当前结果推入历史
- 为什么 undo/redo 后通常可以直接显示，不必再走一遍完整重建


## 1. 先说结论

当前代码里的基本机制是：

```text
tool 运行中
-> 直接修改当前工作文档 m_doc
-> 做局部 solve / 局部重绘
-> 但不 push history

tool 提交 COMMIT
-> rebuild_internal()
-> update_pending()
-> history_push()
-> 形成一条 undo/redo 历史记录

tool 取消 REVERT
-> m_doc 恢复到最近一次历史快照

全局 undo/redo
-> 直接加载历史里的 Document 快照
-> 发 rebuilt 信号
-> UI/Renderer 直接显示恢复后的结果
```

如果只记一句：

- tool 会先改当前工作文档；只有提交后才进入历史，所以一次完整操作通常只对应一条 undo


## 2. tool 什么时候改文档

`Editor` 本身主要负责把鼠标事件转给 tool。

`src/editor/editor.cpp`

```cpp
void Editor::handle_cursor_move()
{
    if (m_core.tool_is_active()) {
        ToolArgs args;
        args.type = ToolEventType::MOVE;
        ToolResponse r = m_core.tool_update(args);
        tool_process(r);
    }
}
```

真正修改文档，发生在具体 tool 的 `update()` 里。  
以拖动工具 `ToolMove` 为例：

`src/core/tools/tool_move.cpp`

```cpp
ToolResponse ToolMove::update(const ToolArgs &args)
{
    auto &doc = get_doc();
    auto &last_doc = m_core.get_current_last_document();
    if (args.type == ToolEventType::MOVE) {
        for (auto [entity, point] : m_entities) {
            ...
            en_movable->move(en_last, delta2d, point);
            ...
            en_movable3d->move(en_last, delta, point);
        }

        doc.set_group_solve_pending(m_first_group);
        m_core.solve_current(m_dragged_list);
        ...
        return ToolResponse();
    }
}
```

这里可以看到两件事：

- `en_movable->move(...)` / `en_movable3d->move(...)` 直接修改当前文档里的 entity
- `doc.set_group_solve_pending(...)` + `m_core.solve_current(...)` 会触发局部求解和局部更新

所以：

- tool 在运行过程中就会实时修改当前 `Document`
- 不是等到最后提交时才第一次改文档


## 3. 当前被修改的是哪份文档

当前正在编辑的文档是 `DocumentInfo::m_doc`。

`src/core/core.cpp`

```cpp
Document &Core::get_current_document()
{
    return get_current_document_info().get_document();
}
```

而 `ToolMove::update()` 里拿到的：

```cpp
auto &doc = get_doc();
```

最终就是当前工作的这份 `m_doc`。

同一个函数里还会拿：

```cpp
auto &last_doc = m_core.get_current_last_document();
```

这不是当前工作文档，而是“最近一次已经进入历史的文档快照”。  
拖动时常见模式是：

- 以 `last_doc` 作为稳定基准
- 把这次鼠标位移作用到当前 `m_doc`

这样能避免一帧叠一帧的累计误差。


## 4. 为什么拖动时会实时变化，但不会立刻产生很多条 undo

因为拖动时虽然改了 `m_doc`，但还没有 push 到 history。

历史入栈发生在：

`src/core/core.cpp`

```cpp
void Core::rebuild_finish(bool from_undo, const std::string &comment)
{
    if (!from_undo) {
        m_documents.at(m_current_document).history_push(comment);
    }
    m_signal_rebuilt.emit();
}
```

而 `rebuild_finish()` 是在 tool 返回 `COMMIT` 后走到的：

`src/core/core.cpp`

```cpp
bool Core::maybe_end_tool(const ToolResponse &r)
{
    if (r.result == ToolResponse::Result::COMMIT) {
        const auto comment = "tool";
        rebuild_internal(false, comment);
        set_needs_save(true);
    }
}
```

所以拖动阶段的真实状态是：

- 当前 `m_doc` 已经在变
- 画面也跟着变
- 但历史栈还没新增节点

这就是为什么：

- 鼠标拖 100 次，不会产生 100 条 undo
- 最终提交后，通常只新增 1 条历史记录


## 5. commit 时到底做了什么

tool 返回：

```cpp
return ToolResponse::commit();
```

之后主链是：

```text
ToolResponse::commit()
-> Core::maybe_end_tool()
-> Core::rebuild_internal()
-> Document::update_pending()
-> Core::rebuild_finish()
-> history_push()
```

关键代码：

`src/core/core.cpp`

```cpp
void Core::rebuild_internal(bool from_undo, const std::string &comment)
{
    ...
    get_current_document().update_pending();
    ...
    rebuild_finish(from_undo, comment);
}
```

这里的含义是：

- 提交时先把当前工作文档做正式重建
- 把 pending 的 generate / solve / solid model 更新跑完
- 然后把“这次提交后的最终文档状态”推入历史


## 6. cancel / revert 为什么能直接回去

tool 取消时通常返回：

```cpp
return ToolResponse::revert();
```

然后：

`src/core/core.cpp`

```cpp
else if (r.result == ToolResponse::Result::REVERT) {
    get_current_document_info().revert();
    rebuild_internal(true, "undo");
}
```

`revert()` 本体很直接：

`src/core/core.cpp`

```cpp
void Core::DocumentInfo::revert()
{
    m_doc.reset();
    m_doc.emplace(get_last_document());
}
```

意思就是：

- 先把当前正在被 tool 改动的 `m_doc` 丢掉
- 再用最近一次历史快照重新构造一份 `m_doc`

所以未提交的拖动修改虽然改了文档，但只是临时工作态：

- commit：保留并进入历史
- revert：整份丢弃，回到上一个历史快照


## 7. undo/redo 机制不是“反向执行操作”，而是“加载文档快照”

历史项存的不是“某次操作指令”，而是一整份 `Document`。

`src/core/core.cpp`

```cpp
class HistoryItemDocument : public HistoryManager::HistoryItem {
public:
    HistoryItemDocument(const Document &doc, const std::string &cm)
        : HistoryManager::HistoryItem(cm), document(doc)
    {
    }
    Document document;
};
```

入栈：

```cpp
void Core::DocumentInfo::history_push(const std::string &comment)
{
    m_history_manager.push(std::make_unique<HistoryItemDocument>(m_doc.value(), comment));
}
```

撤销 / 重做：

```cpp
bool Core::DocumentInfo::undo()
{
    if (!m_history_manager.can_undo())
        return false;
    history_load(m_history_manager.undo());
    return true;
}
```

```cpp
bool Core::DocumentInfo::redo()
{
    if (!m_history_manager.can_redo())
        return false;
    history_load(m_history_manager.redo());
    return true;
}
```

加载历史项：

```cpp
void Core::DocumentInfo::history_load(const HistoryManager::HistoryItem &it)
{
    auto &itd = dynamic_cast<const HistoryItemDocument &>(it);
    m_doc.reset();
    m_doc.emplace(itd.document);
    m_needs_save = true;
}
```

所以它不是：

- 把“移动点”“加约束”“拉伸”等操作反着执行一遍

而是：

- 直接把当时保存下来的整份 `Document` 恢复出来


## 8. 为什么 undo/redo 后通常不用再完整重算也能显示

全局 undo/redo 代码是：

`src/core/core.cpp`

```cpp
void Core::undo()
{
    if (get_current_document_info().undo()) {
        fix_current_group();
        update_can_close();
        m_signal_rebuilt.emit();
        m_signal_needs_save.emit();
    }
}
```

```cpp
void Core::redo()
{
    if (get_current_document_info().redo()) {
        fix_current_group();
        update_can_close();
        m_signal_rebuilt.emit();
        m_signal_needs_save.emit();
    }
}
```

这里没有再调用：

```cpp
get_current_document().update_pending();
```

也就是说，标准 undo/redo 路径并不是：

- 先恢复草图/特征参数
- 再从头 generate / solve / update_solid_model 一遍

而是：

- 直接恢复一份已经保存好的 `Document` 快照
- 然后发 `m_signal_rebuilt.emit()`，通知 UI 刷新

之所以通常能直接显示，是因为历史快照里的 `Document` 不只包含：

- `m_groups`
- `m_entities`
- `m_constraints`

也包含了当时已经算好的 group 状态、solid model 指针以及渲染会用到的缓存状态。  
所以恢复后，Renderer 往往可以直接消费这些结果。


## 9. 一张最小流程图

### 9.1 tool 运行中

```text
鼠标移动
-> Editor::handle_cursor_move()
-> Core::tool_update()
-> ToolMove::update()
-> 直接修改 m_doc
-> 局部 solve / 局部重绘
-> 不 push history
```

### 9.2 tool 提交

```text
ToolResponse::commit()
-> Core::maybe_end_tool()
-> rebuild_internal()
-> update_pending()
-> rebuild_finish()
-> history_push()
```

### 9.3 tool 取消

```text
ToolResponse::revert()
-> DocumentInfo::revert()
-> m_doc = 最近一次历史快照
```

### 9.4 全局 undo / redo

```text
HistoryManager::undo()/redo()
-> history_load()
-> m_doc = 历史快照
-> m_signal_rebuilt.emit()
```


## 10. 总结

- tool 在 `update()` 阶段就会实时修改当前工作文档 `m_doc`
- 拖动时虽然文档不断变化，但这些变化在 commit 前不会进入历史栈
- tool 返回 `COMMIT` 后，系统会先正式重建，再把当前 `Document` 推入历史
- tool 返回 `REVERT` 时，会直接丢弃当前 `m_doc`，恢复到最近一次历史快照
- undo/redo 不是按操作反向执行，而是直接加载整份 `Document` 快照
- 也正因为历史里存的是完整文档状态，所以 undo/redo 后通常可以直接显示，而不是必须重新完整求解和建模
