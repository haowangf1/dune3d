# group/entity/Toposhape

这三个概念分属三层，不是同一个东西：

- `Entity`：文档里的几何对象，负责表达草图/参考几何/可编辑参数
- `Group`：建模步骤和组织单元，负责定义“这一批几何在当前步骤里怎么被解释和处理”
- `TopoDS_Shape`：OCC 内核里的 B-rep 实体结果，负责实体建模、布尔、离散化、导出

最重要的一句：

- `Entity` 不是 `TopoDS_Shape`
- `Group` 也不是 `TopoDS_Shape`
- `Group` 是连接文档几何和 OCC 实体结果的桥


## 1. 在 Document 里怎么存

`Document` 里并列存三大类主数据：实体、约束、组。

`src/document/document.hpp`

```cpp
class Document {
public:
    std::map<UUID, std::unique_ptr<Entity>> m_entities;
    std::map<UUID, std::unique_ptr<Constraint>> m_constraints;

private:
    std::map<UUID, std::unique_ptr<Group>> m_groups;
    ...
};
```

这说明：

- `Entity` 不是 `Group` 的成员数组
- `Group` 也不是 `Entity` 的父类容器
- 两者都直接挂在 `Document` 上

文档序列化时也是分别写出去：

`src/document/document.cpp`

```cpp
json Document::serialize() const
{
    auto j = json{{"type", "document"}};
    m_version.serialize(j);
    {
        auto o = json::object();
        for (const auto &[uu, it] : m_groups) {
            o[uu] = it->serialize(*this);
        }
        j["groups"] = o;
    }
    {
        auto o = json::object();
        for (const auto &[uu, it] : m_entities) {
            if (it->m_kind == ItemKind::USER)
                o[uu] = it->serialize();
        }
        j["entities"] = o;
    }
    {
        auto o = json::object();
        for (const auto &[uu, it] : m_constraints) {
            o[uu] = it->serialize();
        }
        j["constraints"] = o;
    }
    return j;
}
```

这里还能看出一点：

- 文件里主要保存用户数据
- `Group`/`Entity` 的一些运行时派生结果，不一定直接序列化


## 2. Entity 是什么

`Entity` 是文档里的几何对象层。它强调的是参数、点位、归属关系、可编辑性。

`src/document/entity/entity.hpp`

```cpp
class Entity {
public:
    UUID m_uuid;
    virtual Type get_type() const = 0;

    virtual double get_param(unsigned int point, unsigned int axis) const = 0;
    virtual void set_param(unsigned int point, unsigned int axis, double value) = 0;

    virtual glm::dvec3 get_point(unsigned int point, const Document &doc) const;

    UUID m_group;
    std::string m_name;
    bool m_construction = false;

    ItemKind m_kind = ItemKind::USER;
    UUID m_generated_from;
    bool m_visible = true;
};
```

它的职责是：

- 表达 2D 草图几何：`Line2D` / `Arc2D` / `Circle2D` / `Bezier2D`
- 表达 3D/参考几何：`Line3D` / `Workplane` / `STEP` 等
- 作为约束求解的输入对象
- 作为编辑、拖拽、选择、显示的对象

所以 `Entity` 更像“参数化输入几何”，不是 OCC 实体。


## 3. Group 是什么

`Group` 是建模步骤和更新单位，不直接存几何列表，而是存“这一阶段的规则和状态”。

`src/document/group/group.hpp`

```cpp
class Group {
public:
    UUID m_uuid;
    virtual Type get_type() const = 0;

    std::string m_name;
    int m_dof = -1;
    SolveResult m_solve_result = SolveResult::OKAY;

    std::optional<Body> m_body;
    UUID m_active_wrkpl;
    ...
};
```

它的职责是：

- 表达建模历史里的一个步骤：`Sketch` / `Extrude` / `Revolve` / `Loft` ...
- 作为文档更新、求解、实体重建的基本单位
- 定义本步骤如何解释和使用相关 entity

`Group` 不直接保存 `std::vector<Entity*>`。它和 `Entity` 的连接靠 `Entity::m_group`。


## 4. Group 和 Entity 的关系

每个 entity 都通过 `m_group` 归属到某个 group。

`src/document/entity/entity.hpp`

```cpp
UUID m_group;
```

所以关系更像数据库外键：

- `Document` 全局存所有 entity
- 每个 entity 记录自己属于哪个 group
- group 需要几何时，再按 `m_group` 去筛

比如 `GroupExtrude::generate()` 里就是直接扫文档实体：

`src/document/group/group_extrude.cpp`

```cpp
for (const auto &[uu, it] : doc.m_entities) {
    if (it->m_group != m_source_group)
        continue;
    if (it->m_construction)
        continue;
    ...
}
```

这段很关键，说明：

- group 不自己维护 entity 列表
- 而是从 `Document::m_entities` 中按 `m_group`/来源 group 过滤

另外，group 不只使用用户创建的 entity，也会生成自己的派生 entity。

还是 `src/document/group/group_extrude.cpp`

```cpp
auto &new_line = doc.get_or_add_entity<EntityLine3D>(new_line_uu);
new_line.m_p1 = wrkpl.transform(li.m_p1) + dvec;
new_line.m_p2 = wrkpl.transform(li.m_p2) + dvec;
new_line.m_group = m_uuid;
new_line.m_generated_from = uu;
new_line.m_kind = ItemKind::GENRERATED;
```

这里生成的是：

- 拉伸后的辅助 3D 线/轮廓
- 仍然是 `Entity`
- 但属于当前 extrude group
- 不是 OCC 实体本身


## 5. TopoDS_Shape 在哪里

真正的 OCC 实体结果，不在 `Entity` 里，而在 `solid_model` 层。

`src/document/solid_model/solid_model_occ.hpp`

```cpp
class SolidModelOcc : public SolidModel {
public:
    TopoDS_Shape m_shape;
    TopoDS_Shape m_shape_acc;
    Color m_color;
    ...
};
```

这里：

- `m_shape`：当前 group 这一步自己生成的 shape
- `m_shape_acc`：叠加/布尔到前面结果后的累计 shape

这才是 OCC 内核真正关心的 B-rep 数据。


## 6. 三者怎么协作

主链路可以概括成：

```text
Entity -> Group -> SolidModelOcc -> TopoDS_Shape -> triangulate -> render
```

具体含义：

1. `Entity` 提供草图/参考几何输入
2. `Group` 决定如何用这些几何做特征
3. `SolidModelOcc` 调 OCC API 生成 `TopoDS_Shape`
4. `TopoDS_Shape` 再被离散成渲染数据

最典型的是拉伸。


## 7. 以 Extrude 为例：从 Entity 到 TopoDS_Shape

### 7.1 Group 发起实体重建

`src/document/group/group_extrude.cpp`

```cpp
void GroupExtrude::update_solid_model(const Document &doc)
{
    m_solid_model = SolidModel::create(doc, *this);
}
```

意思是：

- 当前 extrude group 进入实体建模阶段
- 把 document 和自己交给 `solid_model` 层


### 7.2 从 document 中提取 source group 的草图几何

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
auto face_builder = FaceBuilder::from_document(doc, group.m_wrkpl, group.m_source_group, offset);
```

这一步会：

- 从 `group.m_source_group` 找到输入草图 entity
- 限定在 `group.m_wrkpl` 上
- 把闭合轮廓组装成 OCC face


### 7.3 把 2D entity 变成 OCC edge / wire / face

`src/document/solid_model/solid_model_util.cpp`

```cpp
if (auto arc = dynamic_cast<const EntityArc2D *>(&edge.entity)) {
    auto new_edge = BRepBuilderAPI_MakeEdge(garc, sa, ea);
    wire.Add(new_edge);
}
else if (auto bezier = dynamic_cast<const EntityBezier2D *>(&edge.entity)) {
    Handle(Geom_BezierCurve) curve = new Geom_BezierCurve(poles);
    auto new_edge = BRepBuilderAPI_MakeEdge(curve);
    wire.Add(new_edge);
}
else {
    auto new_edge = BRepBuilderAPI_MakeEdge(gp_Pnt(pat.x, pat.y, pat.z), gp_Pnt(pbt.x, pbt.y, pbt.z));
    wire.Add(new_edge);
}
```

然后 wire 变成 face：

```cpp
BRepBuilderAPI_MakeFace make_face(wire);
...
m_builder.Add(m_compound, make_face.Face());
```

这一步很关键：

- `Entity` 在这里被“翻译”到 OCC 拓扑/几何对象
- 但它们并不是预先就以 `TopoDS_Shape` 形式存在


### 7.4 OCC 生成当前特征自己的 shape

`src/document/solid_model/solid_model_extrude.cpp`

```cpp
mod->m_shape = BRepPrimAPI_MakePrism(face_builder.get_faces(), gp_Vec(dvec.x, dvec.y, dvec.z));
```

这里：

- 输入是 face
- 操作是 prism/extrude
- 输出是当前 group 的 `m_shape`


### 7.5 累加到前序 body 结果

`src/document/solid_model/solid_model_occ.cpp`

```cpp
bool SolidModelOcc::update_acc_finish(const Document &doc, const Group &group)
{
    auto operation = dynamic_cast<const IGroupSolidModel &>(group).get_operation();
    const auto last_solid_model = dynamic_cast<const SolidModelOcc *>(get_last_solid_model(doc, group));
    update_acc(operation, last_solid_model);
    ...
    finish(doc, group);
    return true;
}
```

如果没有前序实体：

```cpp
void SolidModelOcc::update_acc(IGroupSolidModel::Operation op, const SolidModelOcc *last)
{
    if (last)
        update_acc(op, last->m_shape_acc);
    else
        m_shape_acc = m_shape;
}
```

如果有前序实体，则通过布尔运算得到新的累计体：

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


## 8. TopoDS_Shape 怎么进入渲染

`TopoDS_Shape` 不是直接给 OpenGL 画，而是先三角化。

`src/document/solid_model/solid_model_occ.cpp`

```cpp
void SolidModelOcc::finish(const Document &doc, const Group &group)
{
    ...
    triangulate();
    find_edges();
}
```

`triangulate()`：

```cpp
void SolidModelOcc::triangulate()
{
    m_faces.clear();
    Triangulator tri{m_shape_acc, m_color, m_faces};
}
```

真正的 OCC 离散化：

```cpp
Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);

if (triangulation.IsNull() || triangulation->Deflection() > USER_PREC + Precision::Confusion())
    isTessellate = Standard_True;

if (isTessellate) {
    BRepMesh_IncrementalMesh IM(face, USER_PREC, Standard_False, USER_ANGLE);
    triangulation = BRep_Tool::Triangulation(face, loc);
}
```

然后填进渲染数据：

```cpp
face_out.vertices.push_back(vertex);
face_out.normals.emplace_back(vt.x, vt.y, vt.z);
face_out.triangle_indices.emplace_back(a - 1, b - 1, c - 1);
```

所以完整后半段是：

- `TopoDS_Shape`
- OCC 三角化
- `face::Faces`
- 渲染器绘制


## 9. 一张关系图

```text
Document
├─ m_groups
│   ├─ GroupSketch
│   ├─ GroupExtrude
│   └─ GroupRevolve
│
├─ m_entities
│   ├─ EntityLine2D
│   ├─ EntityArc2D
│   ├─ EntityCircle2D
│   ├─ EntityLine3D      (generated)
│   └─ EntityWorkplane
│
└─ m_constraints

Entity
  - 参数化几何/输入几何
  - 通过 m_group 归属某个 Group

Group
  - 建模步骤/更新单位
  - 读取相关 Entity
  - 调用 solid_model 生成实体结果

SolidModelOcc
  - 持有 m_shape / m_shape_acc
  - 内部使用 OCC TopoDS_Shape
  - 三角化后提供渲染数据
```


## 10. 总结

这三者的关系可以压缩成下面几句：

- `Entity` 是文档层几何对象，负责表达和编辑
- `Group` 是建模步骤，负责组织、解释、求解、重建
- `TopoDS_Shape` 是 OCC 实体结果，负责真正的实体建模和布尔

协作方式是：

- `Document` 保存 `Group` 和 `Entity`
- `Entity::m_group` 把 entity 归属到某个 group
- `Group` 在重建时从 document 中读取相关 entity
- `solid_model` 层把这些 entity 转成 OCC face/wire/shape
- 最终得到 `TopoDS_Shape`，再离散化后渲染

所以不要把它们混成一层：

- `Entity` 不是 `TopoDS_Shape`
- `Group` 也不是 `TopoDS_Shape`
- `Group + solid_model` 才是文档几何进入 OCC 的桥
