# 复杂CSG交互性能优化思路_参考OCC

这篇文档讨论两个问题：

- FreeCAD 这类 CAD 在“复杂 CSG 拖慢无关编辑”问题上，主要怎么缓解
- 基于 dune3d 当前的线性特征架构，我们现实能做哪些优化

先说结论：

- FreeCAD 的核心优势不是“隐藏对象就不重算”
- 而是：
  - 依赖图驱动的局部 recompute
  - 可跳过自动 recompute 的工作流
  - 渲染层缓存
- dune3d 现在做不了 FreeCAD 那种完整 DAG 依赖闭包剪枝
- 但仍然可以做几类很现实的优化，尤其是：
  - body 级 solid model 更新剪枝
  - 延迟三角化
  - 把“显示隐藏”和“是否参与重建”分开


## 1. 这个问题的本质

当前 issue 说的是：

- 有些复杂操作，比如 array 后再做 CSG
- 即使这些 body 被设成 invisible
- 无关的草图编辑仍然会变慢

这说明当前瓶颈不是单纯“有没有画出来”，而更可能是：

- 文档重建范围太大
- OCC 实体更新太大
- 三角化和边提取重复发生

也就是说：

- `visible = false` 不等于“不参与后续计算”


## 2. FreeCAD 主要靠什么缓解

### 2.1 不是靠“隐藏就不算”

FreeCAD 里 `Visibility` 更多是 GUI / ViewProvider 层概念，不等于 App 层 recompute 剪枝条件。

换句话说：

- 隐藏对象主要影响显示
- 不天然等于“跳过建模重算”

所以如果一个昂贵对象还在依赖链上：

- 即使隐藏
- 它也可能仍然参与 recompute


### 2.2 真正更重要的是依赖图驱动的局部 recompute

FreeCAD 的对象依赖不是简单线性后缀，而是显式依赖传播。

效果上就是：

- 改了 A
- 只重算依赖 A 的对象
- 不依赖 A 的对象不动

这比 dune3d 当前的：

- 从某个 group 起点开始
- 后缀全部往后跑

要更精确。

所以 FreeCAD 真正强的地方是：

- **重算范围剪枝**

不是：

- 隐藏对象自动跳算


### 2.3 还有“Skip recomputes / 手动 recompute”工作流

FreeCAD 还提供了非常务实的手段：

- 编辑期间先不自动 recompute
- 改完再手动 recompute

这类机制的意义是：

- 不要求系统每一步都足够聪明
- 而是给用户一个“先攒修改、最后一次性算”的开关


### 2.4 渲染层还有缓存，但这主要解决“画得慢”

FreeCAD 3D 视图用的是 Coin3D / OpenInventor 场景图缓存思路。

它主要改善的是：

- 场景遍历
- OpenGL 提交
- 重复 redraw

但它并不直接减少：

- OCC 布尔计算
- shape 更新
- topo 到 mesh 的重新生成

所以要分清：

- render cache 解决的是显示层成本
- recompute 剪枝解决的是建模层成本


## 3. dune3d 为什么做不了 FreeCAD 那种完整优化

原因不是 OCC 不行，而是当前 dune3d 的数据架构不同。

### 3.1 当前重建机制是线性后缀

你前面已经看过，`Document::update_pending()` 的基本逻辑是：

- group 按顺序排序
- 从 `m_first_group_*` 开始
- 往后跑 `generate / solve / update_solid_model`

本质是：

- 线性历史链
- 不是显式依赖图

所以当前系统的默认安全策略是：

- 前面变了
- 后面都有可能受影响
- 那就后缀全跑


### 3.2 依赖关系现在大多是隐式的

当前 group 之间的依赖散落在很多字段里，例如：

- `m_source_group`
- `m_sources`
- `m_wrkpl`
- `m_active_wrkpl`
- body 边界
- group 顺序

但系统并没有统一抽成：

```text
Group A depends on Group B
Group C depends on Group D
```

这样的显式边表。

所以：

- 没法直接做通用 dependency closure
- 除非先做一次比较大的依赖图重构


## 4. 但 dune3d 仍然可以做的优化

虽然做不了完整 DAG 剪枝，但不是完全没办法。

更现实的目标应该是：

- 在现有线性特征架构里，减少明显没必要的后续重算


## 5. 优化方向一：body 级 solid model 更新剪枝

这是我认为最值得先做的。

### 5.1 为什么它现实

因为当前 solid model 链本来就是按 body 分区的。

你前面看过：

```cpp
auto this_body = &group.find_body(doc).body;
...
if (body != this_body)
    continue;
```

这说明：

- 某个 group 的实体累计结果
- 只和本 body 里的前驱实体链相关

所以如果当前编辑只影响 body A：

- body B / C 的 solid model 后缀本来就不该跟着一起跑


### 5.2 能优化什么

可以优先避免：

- 无关 body 的 `update_solid_model()`
- 无关 body 的 `SolidModel::create(...)`
- 无关 body 的 OCC 布尔
- 无关 body 的三角化 / 提边


### 5.3 它不是完整依赖剪枝，但收益会很直接

因为复杂 CSG 的大头通常就在：

- solid model 更新
- 布尔
- 三角化

所以哪怕只把“无关 body 不参与这轮实体更新”做好，收益也会比较明显。


## 6. 优化方向二：延迟三角化

这条你已经单独分析过了，这里只放在整体方案里定位一下。

### 6.1 它解决的不是 OCC 建模，而是离散化成本

当前 `SolidModelOcc::finish()` 里会直接：

- `triangulate()`
- `find_edges()`

所以只要某个 group 的 solid model 被重新创建：

- mesh 和边缓存也会立刻重算

### 6.2 它的价值

延迟三角化的价值是：

- 避免“明明当前并不需要显示这个结果，却先把 mesh 全做了”

它能减少：

- mesh 生成
- OpenGL 准备阶段的成本

### 6.3 它的边界

但它不能解决：

- OCC 本体建模慢
- 无关 body 的布尔也被跑了

所以它是：

- **渲染/离散化层优化**

不是：

- **重建范围优化**


## 7. 优化方向三：把“可见性”与“是否参与重建”分开

当前 issue 已经说明：

- body invisible
- 但仍然拖慢交互

这说明单纯的 `visible` 只是视图层概念。

可以考虑增加独立策略，例如：

- `suspend_rebuild`
- `manual_recompute_only`
- `defer_solid_model_update`

这类开关的意义是：

- 不把“是否显示”误用成“是否计算”
- 而是明确给一个“这段重计算先挂起”的语义

这更接近 FreeCAD 的 “Skip recomputes” 思路。


## 8. 优化方向四：把后缀全跑改成“有限的局部剪枝”

虽然做不了通用 DAG，但仍然可以做弱化版。

例如：

- 某些改动只影响当前 group solve，就不要扩散到所有 body 的 solid model
- 某些纯草图编辑，先只更新当前组和必要前后文
- 只有真正进入实体链时，才继续推进更深的更新

这类优化的本质是：

- 不要求建立完整依赖图
- 但可以利用现有明显的结构边界做剪枝

例如：

- body 边界
- source group 引用
- 当前 group 是否本身为 solid model group


## 9. 一个现实的优化优先级

如果按收益 / 风险比排序，我建议是：

### 第一步：body 级 solid model 更新剪枝

优点：

- 不改整体架构
- 直接砍掉无关 body 的昂贵 OCC / triangulate
- 和当前代码的 body 分区天然匹配

### 第二步：延迟三角化

优点：

- 实现边界清晰
- 对“频繁编辑但暂时不需要显示最新 mesh”的场景有效

### 第三步：引入“跳过自动重建”模式

优点：

- 对超大模型很实用
- 即使系统剪枝还不够智能，用户也能先工作再集中重算

### 第四步：逐步补显式依赖信息

优点：

- 为以后更强的 dependency closure 打基础

缺点：

- 这已经偏架构演进，不再是单次性能修复


## 10. 一个具体例子

假设当前文档是：

```text
Body A:
  Sketch A1
  Extrude A1
  Array A1
  CSG A1   <- 很重

Body B:
  Sketch B1
  Extrude B1
```

现在用户在 `Sketch B1` 里拖一个尺寸。

### 当前较差的情况

可能发生：

- `Sketch B1` 改动
- 后面后缀更新一路跑
- `Body A` 那条重 CSG 链也被带着更新 / 三角化

结果就是：

- 明明在改 Body B 的草图
- 却被 Body A 的复杂布尔拖慢

### 更合理的目标

应该尽量做到：

- `Sketch B1` 的 solve 正常更新
- `Body B` 自己的实体链更新
- `Body A` 那条无关的重 CSG 链不参与这轮 `update_solid_model`
- 如果当前 area 也不需要 `Body B` 的最终 mesh，可继续延迟三角化

这虽然还不是 FreeCAD 式完整依赖闭包，但已经能解决当前 issue 的大部分痛点。


## 11. 总结

- FreeCAD 解决这类问题的核心，不是“隐藏就不算”，而是：
  - 依赖图驱动的局部 recompute
  - 跳过自动 recompute 的工作流
  - 渲染缓存
- dune3d 当前因为是线性 group 架构，做不了完整 dependency closure
- 但仍然可以做几种现实且有价值的优化：
  - body 级 solid model 更新剪枝
  - 延迟三角化
  - 把 visibility 和 recompute policy 分开
  - 在现有线性架构上做有限的局部剪枝

如果只选一个最值得先做的方向：

- **优先做 body 级 `update_solid_model` 剪枝**

因为它最贴近当前 issue 的真实大头，也最符合 dune3d 现有代码结构。
