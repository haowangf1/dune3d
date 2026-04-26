# 复杂CSG交互性能优化思路

这篇文档针对这样一类问题：

- 文档后面有很重的 array / boolean / CSG body
- 当前用户只是在前面做普通草图编辑
- 但每次编辑提交后，后面那些重 body 也跟着同步重建
- 即使 body 被隐藏，也还是明显拖慢交互

这里不讨论“几何算法本身怎么变快”，只讨论基于当前 `dune3d` 代码结构，能做的架构级优化点。


## 1. 现状问题

当前代码里有三个关键事实。

### 1.1 隐藏 body 只影响渲染，不影响重建

`DocumentView` 的 `body_solid_model_visible` 只在渲染阶段生效：

`src/workspace/document_view.cpp`

```cpp
bool DocumentView::body_solid_model_is_visible(const UUID &uu) const
{
    if (m_body_views.contains(uu))
        return m_body_views.at(uu).m_solid_model_visible;
    return true;
}
```

`src/render/renderer.cpp`

```cpp
for (auto body_groups : groups_by_body) {
    if (!m_doc_view->body_solid_model_is_visible(body_groups.get_group().m_uuid))
        continue;
    ...
}
```

但文档重建时并不会看这个状态：

`src/document/document.cpp`

```cpp
if (index >= first_update_solid_model_index) {
    update_solid_model(*group);
}
```

所以：

- 隐藏 body = 不画
- 不等于不重建


### 1.2 tool 拖动阶段可以局部更新，但 commit 后又全量补算

tool 交互时，其实已经支持“只更新到当前 group”：

`src/core/core.cpp`

```cpp
void Core::solve_current(const DraggedList &dragged)
{
    auto &doc = get_current_document();
    doc.update_pending(get_current_group(), dragged);
}
```

但 tool 一旦 `COMMIT`，会走：

`src/core/core.cpp`

```cpp
if (r.result == ToolResponse::Result::COMMIT) {
    rebuild_internal(false, comment);
    set_needs_save(true);
}
```

而 `rebuild_internal()` 是全量：

```cpp
void Core::rebuild_internal(bool from_undo, const std::string &comment)
{
    ...
    get_current_document().update_pending();
    ...
}
```

所以实际效果是：

- 拖动时局部更新
- 一提交，又把后面所有 pending 的重 group 全算一遍


### 1.3 solid model 更新把 shape 构建和渲染缓存准备绑死了

`SolidModelOcc::update_acc_finish()` 最后会直接 `finish()`：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
bool SolidModelOcc::update_acc_finish(const Document &doc, const Group &group)
{
    ...
    finish(doc, group);
    return true;
}
```

而 `finish()` 里固定做：

```cpp
void SolidModelOcc::finish(const Document &doc, const Group &group)
{
    ...
    triangulate();
    find_edges();
}
```

也就是说每次实体重建，不只是：

- 更新 `m_shape` / `m_shape_acc`

还一定会：

- 三角化
- 提取边

这对隐藏 body 和重 body 都很贵。


## 2. 一个具体例子

假设模型结构如下：

```text
Sketch 1
-> Extrude 1
-> Linear Array 1   (复制 200 个实例)
-> Solid Model Operation 1   (和另一个 body 做 boolean)
```

用户现在只是在 `Sketch 1` 里改一条线的尺寸。

当前现状很可能是：

1. 草图求解
2. `Extrude 1` 更新
3. `Linear Array 1` 重新生成全部阵列 shape
4. `Solid Model Operation 1` 重新做 boolean
5. 结果再整体 triangulate / find_edges

即使：

- 当前用户只关心草图
- 后面的 body 被隐藏

这些操作仍然会在提交后被同步执行。


## 3. 优化点二：将 shape 构建和渲染缓存生成拆开

### 思路

当前 `finish()` 同时做：

- shape 完成
- triangulate
- find_edges

可以拆成两层：

1. `shape` 层：只保证 `m_shape` / `m_shape_acc` 可用于后续布尔和下游建模
2. `render cache` 层：按需生成 `m_faces` / `m_edges`

也就是把：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
triangulate();
find_edges();
```

从“每次必做”改成“按需做”。

### 现在造成的不良影响

- 即使 body 不显示
- 即使当前只是交互编辑
- 仍然会为所有重 shape 准备完整显示缓存

这会把：

- 几何结果更新
- 视图准备

两个本可分离的成本绑在一起。

### 预计可优化的内容

- 减少隐藏 body 的无谓 mesh 构建
- 减少交互期间的三角化和边提取开销
- 保留 `m_shape_acc` 供后续实体建模使用

### 应用后的不良影响

这个方案的副作用主要出现在“第一次真正需要显示/选边”时。

#### 1）首次显示该 body 时会出现一次延迟

现在 `finish()` 一次性准备好：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
triangulate();
find_edges();
```

如果拆开后，body 在隐藏阶段只保 shape、不保显示缓存，那么当用户重新打开该 body 的 solid model 显示时，会发生：

- 当次显示前需要补做 triangulate
- 当次显示前可能还要补做 `find_edges`

于是交互体验会从：

- “编辑时卡”

变成：

- “第一次重新显示时卡一下”

这是典型的“把成本从 commit 阶段挪到首次使用阶段”。

#### 2）边相关功能不能再默认假设 `m_edges` 总是现成可用

现在 edge selection 依赖 `last_solid_model->m_edges`：

`src/render/renderer.cpp`

```cpp
for (const auto &[edge_idx, path] : last_solid_model->m_edges) {
    ...
}
```

如果 `find_edges()` 改成按需执行，那么这类功能在进入前必须确保：

- `m_edges` 已经准备好

否则会出现：

- 能显示面，但不能选边
- 或第一次进选边模式时需要补算边缓存

#### 3）需要管理 shape cache 和 render cache 的一致性

当前结构比较简单：

- 只要 `get_solid_model()` 存在，就默认里面的 `m_shape_acc / m_faces / m_edges` 是同一版本

拆开后就会变成：

- shape 是新的
- render cache 可能还是空的或旧的

所以需要额外状态来保证：

- 什么时候 face cache 失效
- 什么时候 edge cache 失效
- 什么时候只重建了 shape 没重建 mesh

否则后面很容易出现“逻辑正确，但缓存使用错版本”的问题。

### 预计优化占比

对 boolean/array 后 shape 很复杂、mesh 很重的场景，粗略预计：

- 20% ~ 45% 的单次更新耗时下降

如果某个场景里 triangulate 比 boolean 还重，这个收益还会更高。

