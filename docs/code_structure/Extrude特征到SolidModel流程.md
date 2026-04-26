# Extrude 特征到 SolidModel 流程

这篇文档总结的是：

- `GroupExtrude::update_solid_model()` 往下，`solid_model` 这一层是怎么工作的
- 这些类是怎么串起来的
- 以 `Extrude` 为例，完整说明从特征参数到最终实体/渲染数据的流程

先说结论：

- `GroupExtrude` 负责保存“拉伸特征参数”
- `solid_model_extrude.cpp` 负责把这些参数变成 OCC shape
- `SolidModelOcc` 负责把当前特征 shape 接到前序 body 实体链上
- 最后再把实体结果转成渲染用的 `m_faces / m_edges`

最核心的链路是：

```text
GroupExtrude
-> SolidModel::create(doc, group)
-> FaceBuilder::from_document(...)
-> BRepPrimAPI_MakePrism(...)
-> SolidModelOcc::update_acc_finish()
-> triangulate() / find_edges()
-> Renderer
```


## 1. 起点：`GroupExtrude`

`GroupExtrude` 在 `group` 层，负责保存拉伸特征的参数，例如：

- `m_source_group`：源草图 group
- `m_wrkpl`：工作平面
- `m_dvec`：拉伸方向/长度向量
- `m_mode`：单向、偏移、对称等模式

它自己并不直接写 OCC 建模细节，而是把工作交给 `solid_model` 层：

`src/document/group/group_extrude.cpp`

```cpp
void GroupExtrude::update_solid_model(const Document &doc)
{
    m_solid_model = SolidModel::create(doc, *this);
}
```

所以第一层职责分工是：

- `GroupExtrude`：保存参数，发起实体重建
- `SolidModel::create(...)`：进入实体建模实现层


## 2. 统一入口：`SolidModel::create`

`SolidModel` 是统一抽象入口。

`src/document/solid_model/solid_model.hpp`

```cpp
class SolidModel {
public:
    static std::shared_ptr<const SolidModel> create(const Document &doc, GroupExtrude &group);
    ...
};
```

对 `Extrude` 来说，实际进入的是：

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
std::shared_ptr<const SolidModel> SolidModel::create(const Document &doc, GroupExtrude &group)
```

这一步开始，控制权从 `group` 层进入 `solid_model` 目录。


## 3. 先把草图轮廓转成 OCC face：`FaceBuilder`

拉伸不是直接拿草图 entity 去调用 OCC，而是先把草图轮廓组装成 face。

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
auto face_builder = FaceBuilder::from_document(doc, group.m_wrkpl, group.m_source_group, offset);
```

这里进入的是：

- `solid_model_util.hpp`
- `solid_model_util.cpp`

`FaceBuilder` 的职责是：

- 从 `Document` 中取出 `source_group` 的草图轮廓
- 结合 `wrkpl` 把 2D 轮廓转换到 3D
- 处理闭合轮廓和孔洞
- 构建 OCC `wire` / `face`

接口定义：

`src/document/solid_model/solid_model_util.hpp`

```cpp
class FaceBuilder {
public:
    static FaceBuilder from_document(const Document &doc, const UUID &wrkpl_uu, const UUID &source_group_uu,
                                     const glm::dvec3 &offset);

    const TopoDS_Compound &get_faces() const;
};
```

所以这一步可以理解成：

- 文档草图输入
-> OCC 可接受的面输入


## 4. 真正生成当前特征的 OCC shape

拿到 face 后，`Extrude` 用 OCC 的 `MakePrism` 生成当前特征自己的 shape。

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
auto mod = std::make_shared<SolidModelOcc>();
...
mod->m_shape = BRepPrimAPI_MakePrism(face_builder.get_faces(), gp_Vec(dvec.x, dvec.y, dvec.z));
```

这里：

- `mod` 是 `SolidModelOcc`
- `mod->m_shape` 表示当前这一步 extrude 自己生成的 shape

这一步还没把它接到整个 body 的历史链上，只是“这一特征自己的结果”。


## 5. 接到 body 实体链上：`update_acc_finish()`

生成 `m_shape` 之后，接着调用：

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
if (!mod->update_acc_finish(doc, group)) {
    ...
    return nullptr;
}
```

进入：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
bool SolidModelOcc::update_acc_finish(const Document &doc, const Group &group)
{
    auto operation = dynamic_cast<const IGroupSolidModel &>(group).get_operation();
    const auto last_solid_model = dynamic_cast<const SolidModelOcc *>(get_last_solid_model(doc, group));
    update_acc(operation, last_solid_model);

    if (m_shape_acc.IsNull()) {
        return false;
    }

    finish(doc, group);

    return true;
}
```

这一步做了三件事：

1. 找当前 body 中前一个可用实体结果
2. 把当前 `m_shape` 接到前一个 `m_shape_acc` 后面
3. 做收尾处理


## 6. 关键公共逻辑：找“上一棒”实体结果

`get_last_solid_model()` / `get_last_solid_model_group()` 的作用是：

- 在当前 body 中
- 按 group 历史顺序
- 找到当前特征前面最近一个已有实体结果的 group

`src/document/solid_model/solid_model.cpp`

```cpp
const IGroupSolidModel *SolidModel::get_last_solid_model_group(const Document &doc, const Group &group,
                                                               IncludeGroup include_group)
{
    const IGroupSolidModel *last_solid_model_group = nullptr;

    auto this_body = &group.find_body(doc).body;

    for (auto gr : doc.get_groups_sorted()) {
        if (include_group == IncludeGroup::NO && gr->m_uuid == group.m_uuid)
            break;
        if (auto gr_solid = dynamic_cast<const IGroupSolidModel *>(gr)) {
            if (auto solid_model = dynamic_cast<const SolidModelOcc *>(gr_solid->get_solid_model())) {
                auto body = &gr->find_body(doc).body;
                if (body != this_body)
                    continue;
                if (!solid_model->m_shape_acc.IsNull())
                    last_solid_model_group = gr_solid;
            }
        }
        if (include_group == IncludeGroup::YES && gr->m_uuid == group.m_uuid)
            break;
    }

    return last_solid_model_group;
}
```

这一步的意义可以直接理解成：

- 给当前特征找“前一个实体前驱”

因为实体累计链是按 body 独立进行的，所以它只在同一个 body 内搜索。


## 7. `m_shape` 和 `m_shape_acc` 的区别

`SolidModelOcc` 里最关键的两个字段是：

`src/document/solid_model/solid_model_occ.hpp`

```cpp
class SolidModelOcc : public SolidModel {
public:
    TopoDS_Shape m_shape;
    TopoDS_Shape m_shape_acc;
    Color m_color;
```

含义分别是：

- `m_shape`：当前特征自己生成的 shape
- `m_shape_acc`：接到前序 body 结果后的累计实体结果

可以理解成：

- `m_shape` = “这一步做了什么”
- `m_shape_acc` = “做到这一步为止整个 body 现在是什么样”


## 8. 实体累计是怎么做的

如果当前 body 前面没有实体结果：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
void SolidModelOcc::update_acc(IGroupSolidModel::Operation op, const SolidModelOcc *last)
{
    if (last)
        update_acc(op, last->m_shape_acc);
    else
        m_shape_acc = m_shape;
}
```

也就是：

- `m_shape_acc = m_shape`

如果前面有实体结果，则通过布尔运算接上去：

```cpp
TopoDS_Shape SolidModelOcc::calc(IGroupSolidModel::Operation op, TopoDS_Shape argument, TopoDS_Shape tool)
{
    switch (op) {
    case IGroupSolidModel::Operation::DIFFERENCE:
        return BRepAlgoAPI_Cut(argument, tool);
    case IGroupSolidModel::Operation::UNION:
        return BRepAlgoAPI_Fuse(argument, tool);
    case IGroupSolidModel::Operation::INTERSECTION:
        return BRepAlgoAPI_Common(argument, tool);
    }
}
```

所以这一步的本质是：

- 当前特征 `m_shape`
- 接到前一个 body 累计实体 `m_shape_acc`
- 得到当前新的 `m_shape_acc`


## 9. 收尾：转成渲染和边选择数据

实体链接好以后，`finish()` 会做后处理：

`src/document/solid_model/solid_model_occ.cpp`

```cpp
void SolidModelOcc::finish(const Document &doc, const Group &group)
{
    auto body = group.find_body(doc).body;
    if (body.m_color) {
        m_color = *body.m_color;
    }
    else {
        m_color = Preferences::get().canvas.appearance.get_color(ColorP::SOLID_MODEL);
    }

    triangulate();
    find_edges();
}
```

这里做三件事：

1. 决定颜色
2. `triangulate()`：把 `m_shape_acc` 转成网格面
3. `find_edges()`：提取边离散数据

### 9.1 网格面

```cpp
void SolidModelOcc::triangulate()
{
    m_faces.clear();
    Triangulator tri{m_shape_acc, m_color, m_faces};
}
```

### 9.2 边数据

```cpp
void SolidModelOcc::find_edges()
{
    m_edges.clear();
    TopExp_Explorer topex(m_shape_acc, TopAbs_EDGE);
    ...
}
```

所以 `SolidModelOcc` 最后会同时拥有：

- OCC 实体结果：`m_shape`, `m_shape_acc`
- 渲染数据：`m_faces`
- 边选择数据：`m_edges`


## 10. Renderer 怎么消费这些结果

渲染器并不直接处理 `TopoDS_Shape`，而是使用 `SolidModel` 已经准备好的数据。

`src/render/renderer.cpp`

```cpp
if (gr->get_solid_model()) {
    last_solid_model = gr->get_solid_model();
    last_solid_model_group = group;
}
```

最后画面：

```cpp
m_ca.add_face_group(last_solid_model->m_faces, {0, 0, 0},
                    glm::quat_identity<float, glm::defaultp>(), color);
```

所以 renderer 看到的是：

- `m_faces`
- `m_edges`

而不是原始 OCC shape。


## 11. 一个最简单的建模例子

假设有这样一条 group 历史：

```text
Reference
Sketch 1
Extrude 1
Fillet 1
```

现在系统在更新 `Extrude 1`。

### 第一步：读取参数

`GroupExtrude` 里已经有：

- `m_source_group = Sketch 1`
- `m_wrkpl = 当前工作平面`
- `m_dvec = 拉伸向量`

### 第二步：调用 `update_solid_model()`

```cpp
m_solid_model = SolidModel::create(doc, *this);
```

### 第三步：从 `Sketch 1` 构造 face

```cpp
auto face_builder = FaceBuilder::from_document(doc, group.m_wrkpl, group.m_source_group, offset);
```

### 第四步：OCC 拉伸

```cpp
mod->m_shape = BRepPrimAPI_MakePrism(face_builder.get_faces(), gp_Vec(dvec.x, dvec.y, dvec.z));
```

### 第五步：找前驱实体

这时前面没有已有实体结果，所以：

- `get_last_solid_model(...)` 返回 `nullptr`

### 第六步：当前 extrude 成为 body 的第一个累计实体

```cpp
m_shape_acc = m_shape;
```

### 第七步：三角化和提边

```cpp
triangulate();
find_edges();
```

### 第八步：后续 `Fillet 1` 再更新时，就会把 `Extrude 1` 当作前驱实体

也就是说：

- `Extrude 1` 先产出 body 的初始实体
- `Fillet 1` 再在这个实体上继续加工


## 12. 整体串联关系图

```text
GroupExtrude
  - 保存拉伸参数
  - 调用 update_solid_model()

GroupExtrude::update_solid_model()
  -> SolidModel::create(doc, group)

solid_model_extrude.cpp
  -> FaceBuilder::from_document(...)
  -> BRepPrimAPI_MakePrism(...)
  -> 得到 SolidModelOcc::m_shape

SolidModelOcc::update_acc_finish()
  -> get_last_solid_model(...)
  -> 生成 m_shape_acc
  -> finish()

SolidModelOcc::finish()
  -> triangulate() -> m_faces
  -> find_edges()  -> m_edges

Renderer
  -> 使用 m_faces / m_edges 绘制
```


## 13. 总结

`Extrude` 这条链可以压缩成几句：

- `GroupExtrude` 只负责保存拉伸特征参数
- `solid_model_extrude.cpp` 负责把草图轮廓变成 OCC 拉伸结果
- `SolidModelOcc` 负责把当前特征结果接到 body 实体链上
- `m_shape` 表示当前特征自己的 shape
- `m_shape_acc` 表示到当前为止整个 body 的累计实体
- 最后通过 `triangulate()` 和 `find_edges()` 转成渲染/选择所需数据

所以 `solid_model` 这一层的核心任务就是：

- 把特征参数变成 OCC 实体
- 再把 OCC 实体变成文档后续和渲染都能使用的统一结果
