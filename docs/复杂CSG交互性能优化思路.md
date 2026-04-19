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


## 3. 优化点一：tool commit 后不要立刻全量重建全部后续 group

### 思路

利用现在已经存在的能力：

- `update_pending(last_group)` 可以只更新到指定 group
- 后面的 group 可以继续保持 pending

所以对普通草图/约束编辑类 tool，可以考虑：

- 在交互和提交阶段，只更新到当前 group 或当前 body
- 把后面的重 group 保持为 pending
- 在真正需要时再补算

当前代码已经支持“后面的 pending 继续保留”：

`src/document/document.cpp`

```cpp
if (last_group && last_group->m_uuid == last_group_to_update) {
    if (m_first_group_solve)
        m_first_group_solve = group->m_uuid;
    if (m_first_group_update_solid_model)
        m_first_group_update_solid_model = group->m_uuid;
    return;
}
```

### 现在造成的不良影响

- 前面一个轻量 sketch edit
- 会触发后面全部重特征同步重建
- 用户会感觉“无关草图也很卡”

### 预计可优化的内容

- 大幅减少“编辑前面草图时”被连带重算的后续 group 数量
- 把重算推迟到真正需要最终实体时

### 应用后的不良影响

这个方案的副作用主要不是“算错”，而是“文档处于部分最新、部分未最新”的状态。

结合当前代码，最直接的影响有三类：

#### 1）后续 body 的显示结果会暂时过期

因为 `Renderer` 绘制 body solid model 时，会直接拿各 body 当前已有的最后一个 `get_solid_model()`：

`src/render/renderer.cpp`

```cpp
if (gr->get_solid_model()) {
    last_solid_model = gr->get_solid_model();
    last_solid_model_group = group;
}
```

如果后续 group 被保留为 pending，但没立即重建，那么：

- 视图里看到的后续实体会暂时还是旧结果
- 用户刚改完前面草图时，后面的 body 不一定立刻跟着变化

#### 2）导出、投影视图、选边等功能必须先补全 pending

当前导出逻辑直接读取现成 solid model：

`src/editor/editor_export.cpp`

```cpp
if (gr->get_solid_model())
    last_solid_model = gr->get_solid_model();
```

如果这里不先强制 finalize，可能出现：

- 导出的 STL/STEP 还是旧实体
- 投影视图基于旧模型
- 选实体边模式拿到旧边集

所以这个优化落地后，必须统一规定：

- 导出
- 投影
- edge selection
- 保存前需要最新实体的操作

先显式补跑 `update_pending()`

#### 3）用户心智会从“提交即全局一致”变成“提交即局部一致”

当前 `COMMIT` 后会立刻：

`src/core/core.cpp`

```cpp
rebuild_internal(false, comment);
```

而 `rebuild_internal()` 会执行：

```cpp
get_current_document().update_pending();
```

所以现在的默认语义其实是：

- 只要提交，整个文档实体尽量立刻一致

如果改成局部更新，用户会遇到：

- 当前草图是新的
- 后续 body 还是旧的

这就需要 UI 上明确给出：

- 某些 body 是 stale / pending
- 或在切换过去时自动补算

### 预计优化占比

对“前轻后重”的典型文档，预计最有价值，粗略可贡献：

- 40% ~ 70% 的交互等待时间下降

这里的前提是：

- 当前慢的主要来源确实在后面那些重 group


## 4. 优化点二：将 shape 构建和渲染缓存生成拆开

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


## 5. 优化点三：隐藏 body 时跳过 solid model 显示缓存更新

### 思路

基于当前已有的视图状态：

- `body_solid_model_visible`

对“当前不可见的 body”，至少可以做到：

- 不生成 `m_faces`
- 不生成 `m_edges`

这个优化和上一条很接近，但这里强调的是：

- 直接把“隐藏状态”纳入运行时缓存更新策略

这样做到：

- 可见性不只是 render 阶段跳过
- 还影响 solid model 显示缓存是否需要更新

### 现在造成的不良影响

- 用户明明已经把重 body 隐藏
- 但系统仍然做完整显示准备
- “隐藏了还是慢”的体感非常差

### 预计可优化的内容

- 对隐藏 body 的交互开销显著下降
- 用户隐藏重对象后，能真正换来更流畅的 sketch/edit 体验

### 应用后的不良影响

这个方案相比上一条多了一层“视图状态参与运行时策略”，副作用主要体现在行为复杂度上。

#### 1）可见性从“纯显示概念”变成“影响运行时更新策略”

当前 `body_solid_model_visible` 只是 view 层状态：

`src/workspace/document_view.cpp`

```cpp
bool DocumentView::body_solid_model_is_visible(const UUID &uu) const
```

如果把它用于跳过显示缓存更新，那么它就不再只是：

- 画不画

而变成：

- 要不要准备显示缓存

这样会引入一个新的用户感知差异：

- 只是把 body 隐藏了
- 结果后来再显示时，系统还得补算一次

即“隐藏/显示”不再是纯粹的 UI 动作，而会附带一次潜在重建。

#### 2）切换可见性时可能引发补算尖峰

因为隐藏期间不准备 `m_faces / m_edges`，所以用户重新显示该 body 时：

- 很可能会把之前省掉的代价一次性补回来

如果一个文档里多个重 body 被一起重新打开，就可能出现：

- 平时编辑很顺
- 重新显示时瞬间卡一大下

#### 3）需要明确哪些功能“即使隐藏也必须保 shape”

有些 body 虽然隐藏，但仍可能被后续实体建模引用。

当前 `SolidModelOcc::update_acc_finish()` 的核心依赖是：

```cpp
const auto last_solid_model = dynamic_cast<const SolidModelOcc *>(get_last_solid_model(doc, group));
update_acc(operation, last_solid_model);
```

所以这里不能把“隐藏 body”简单等同于“完全不更新任何 solid model 数据”。

更稳妥的边界是：

- 隐藏 body 可以跳过显示缓存
- 但是否能跳过 shape 更新，要看后续是否还有依赖

也就是说，这个优化落地时必须非常清楚地区分：

- shape 级依赖
- render cache 级依赖

### 预计优化占比

对“隐藏大 body、继续编辑前面草图”的场景，粗略预计：

- 15% ~ 35% 的交互耗时下降

如果文档里有多个大 body 同时隐藏，收益会更明显。


## 6. 优化点四：为 `update_pending()` 增加交互更新策略

### 思路

当前 `update_pending()` 只有一种模式：

`src/document/document.hpp`

```cpp
void update_pending(const UUID &last_group = UUID(), const std::vector<EntityAndPoint> &dragged = {});
```

它无法表达这些差异：

- 这是拖动中的交互更新
- 还是最终一致性重建
- 要不要做 solid model render cache
- 要不要只处理当前 body

可以增加一个“更新策略”概念，例如：

- `Interactive`
- `Finalize`
- `VisibleBodiesOnly`
- `ShapeOnly`

这样同一个入口仍保留，但行为能更细化。

### 现在造成的不良影响

- 所有更新都走一套相对重的路径
- 代码层面无法区分“交互中先给一个轻结果”和“最终必须一致”

### 预计可优化的内容

- 让草图拖动、尺寸编辑、约束调整走轻量更新路径
- 让最终导出/保存/切换时再走完整路径
- 让前台交互和最终结果重建解耦

### 应用后的不良影响

这个方案最大的副作用是：文档更新语义会从“单一模式”变成“多模式”。

#### 1）调用方复杂度会上升

现在谁想更新文档，只需要想一件事：

- 调 `update_pending()`

如果增加策略模式，那么调用方必须知道：

- 我现在是 `Interactive`
- 还是 `Finalize`
- 还是 `ShapeOnly`

这会把原来集中在 `Document` 内部的简单语义，扩散到：

- tool
- editor
- export
- selection

这些调用方。

#### 2）不同模式之间更容易出现行为不一致

例如：

- 拖动时看到的是 shape-only 结果
- 提交时看到的是 partial finalize
- 导出时才是 full finalize

如果边界没定义清楚，就可能出现：

- 某些报错只在 full finalize 阶段出现
- 某些 group 状态消息在轻量模式下不更新
- 某些 UI 看起来“像成功了”，但真正 finalize 才失败

这在当前代码里尤其要小心，因为很多 group 错误信息就是在 `update_solid_model()` 里产生的。

#### 3）测试维度会明显增加

现在只要验证：

- 更新后结果对不对

加策略后要分别验证：

- interactive 是否正确
- finalize 是否正确
- 从 interactive 切到 finalize 是否正确

也就是说，这个方案是很有价值的基础设施改造，但会明显提高维护复杂度。

### 预计优化占比

这条本身更像“总开关式优化基础设施”，单独看可能只有：

- 10% ~ 20%

但它会放大前面三条优化的效果，是非常值得做的基础改造。


## 7. 优化点五：重 array / replicate 在交互阶段延后最终融合

### 思路

当前 replicate/array 逻辑会把每个实例一路 fuse 进最终 shape：

`src/document/solid_model/solid_model_replicate.cpp`

```cpp
for (unsigned int instance = 0; instance < group.get_count(); instance++) {
    auto trsf = make_trsf(instance);
    TopoDS_Shape sh = BRepBuilderAPI_Transform(shape, trsf);
    if (mod->m_shape.IsNull())
        mod->m_shape = sh;
    else
        mod->m_shape = BRepAlgoAPI_Fuse(mod->m_shape, sh);
}
```

对大阵列来说，这非常重。

交互阶段可以考虑：

- 暂缓最终 fuse
- 先保留轻量实例结果或较轻预览表达
- 等到真正 finalize 时再做最终合并

### 现在造成的不良影响

- array 本身就重
- 一旦后面再接 CSG/boolean，会把开销进一步放大
- 前面一个小编辑也会触发整条重链

### 预计可优化的内容

- 明显降低大阵列模型在交互中的更新代价
- 尤其适合“阵列实例很多，但用户当前只在前面草图工作”的场景

### 应用后的不良影响

这个方案的副作用主要来自“交互期结果”和“最终结果”不再完全一致。

#### 1）交互期看到的阵列结果可能不再等于最终结果

当前 replicate 最终结果是通过不断 `Fuse` 得到的：

`src/document/solid_model/solid_model_replicate.cpp`

```cpp
if (mod->m_shape.IsNull())
    mod->m_shape = sh;
else
    mod->m_shape = BRepAlgoAPI_Fuse(mod->m_shape, sh);
```

如果交互阶段延后最终融合，那么交互中看到的可能只是：

- 一组实例
- 或较轻的近似结果

而不是最终 fuse 后的统一 shape。

这会带来一个直接影响：

- 用户在交互中看到的结果更快，但不一定就是最终布尔后的真实拓扑

#### 2）后续依赖该阵列结果的 group 不能盲目继续拿预览结果往下算

如果阵列后面还有：

- boolean
- local operation
- edge selection

这些通常都默认自己拿到的是“最终合法 shape”。

所以这个优化如果落地，必须明确限定场景：

- 只在交互期用于预览/显示
- 真正给后续 group 继续建模前，必须 finalize

否则会把“轻量预览 shape”误用到“正式建模输入”上。

#### 3）实现上需要两套语义：预览阵列结果 vs 最终阵列结果

当前 `GroupReplicate` / `SolidModelOcc` 这条链默认只有一个结果：

- `m_shape`
- `m_shape_acc`

如果做交互阶段延后融合，等于引入：

- preview 结果
- final 结果

这会让 replicate 相关逻辑复杂不少，尤其是在：

- 渲染
- 导出
- 下游特征引用

三个场景之间需要严格区分。

### 预计优化占比

对“大阵列 + 后续 CSG”的典型慢场景，这条往往收益很高，粗略预计：

- 25% ~ 50%

如果阵列数量特别大，收益可能更明显。


## 8. 一个完整例子：这些优化会怎样起作用

还是用前面的例子：

```text
Sketch 1
-> Extrude 1
-> Linear Array 1   (200 instances)
-> Solid Model Operation 1
```

用户现在只改 `Sketch 1` 里的一个尺寸。

### 当前模式

提交后大致会同步做：

1. `Sketch 1` 求解
2. `Extrude 1` 更新
3. `Linear Array 1` 重建全部实例并 fuse
4. `Solid Model Operation 1` 再做 boolean
5. 对结果 triangulate + find_edges

### 应用优化后的模式

可以变成：

1. 提交后只更新到当前 group 或当前 body
2. 后面的 `Linear Array 1` / `Solid Model Operation 1` 保持 pending
3. 如果这些 body 当前隐藏，则不生成 `m_faces` / `m_edges`
4. 如果交互阶段必须略微更新 shape，也只保 `m_shape_acc`，不做完整显示缓存
5. 等用户真正切回那个 body、导出、或显式 rebuild 时，再做最终完整重建

这样用户的体感就会变成：

- 改 sketch 很快
- 重 body 结果稍后再补齐


## 9. 建议的优先级

如果按投入产出比排序，建议优先做：

### 第一优先级

- tool commit 后不要立刻全量重建全部后续 group
- 隐藏 body 时跳过显示缓存更新

这是最容易直接改善 issue 体感的两项。

### 第二优先级

- 拆开 `shape` 更新和 `triangulate/find_edges`

这是后续所有轻量交互更新的基础。

### 第三优先级

- 为 `update_pending()` 增加交互/最终更新策略
- 对 array/replicate 交互阶段做延后融合

这是更进一步的结构优化。


## 10. 总结

基于当前代码结构，这个 issue 最值得做的不是“让 OCC 本身变快”，而是：

- 不要在每次轻量编辑后都把后面所有重 group 同步全算一遍
- 不要对隐藏 body 也无条件生成完整 solid model 显示缓存
- 把 `shape` 结果和渲染缓存拆开
- 让 pending 真正承担“延后重建”的职责

一句话概括：

- 当前最大的问题不是“不会局部更新”，而是“局部更新能力已经有了，但在提交和显示缓存阶段又被重路径吃回去了”
