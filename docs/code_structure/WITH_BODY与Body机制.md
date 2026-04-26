# WITH_BODY 与 Body 机制

这篇文档回答三个问题：

- `WITH_BODY` 在 `dune3d` 里到底是什么意思
- 它影响哪些内容
- 它对应什么工程实际需求

先说结论：

- `WITH_BODY` 不是创建一个 OCC `TopoDS_Shape`
- 它的作用是：把当前新建的 group 标记为一个新 body 的起点
- 从这个 group 开始，后续 group 会归到一个新的 body 链里

最短一句话：

- `WITH_BODY` = 从这里开始新建一个独立实体单元的特征链


## 1. 代码上它到底做了什么

`WITH_BODY` 的核心行为非常简单。

`src/editor/editor_workspace_browser.cpp`

```cpp
if (new_group && add_group_mode == WorkspaceBrowserAddGroupMode::WITH_BODY)
    new_group->m_body.emplace();
```

也就是说：

- 新建 group 时
- 如果模式是 `WITH_BODY`
- 就给这个 group 填上 `m_body`

而 `m_body` 的定义在：

`src/document/group/group.hpp`

```cpp
std::optional<Body> m_body;
```

`Body` 本身只是一个轻量结构：

```cpp
class Body {
public:
    std::string m_name = "Body";
    std::optional<Color> m_color;
    json serialize() const;
};
```

所以这里创建的不是几何实体，而只是：

- 一个 body 起点标记
- 加上一点 body 元数据（名字、颜色）


## 2. Body 是怎么工作的

`dune3d` 不是单独维护一棵 body 树，而是靠 group 顺序和 `m_body` 标记来切分 body。

### 2.1 找当前 group 属于哪个 body

`src/document/group/group.cpp`

```cpp
Group::BodyAndGroup Group::find_body(const Document &doc) const
{
    const Group *body_group = nullptr;
    for (auto group : doc.get_groups_sorted()) {
        if (group->m_body)
            body_group = group;
        if (group == this)
            return {body_group->m_body.value(), *body_group};
    }
    throw std::runtime_error("body not found");
}
```

这段逻辑很关键：

- 按 group 顺序从前往后扫
- 只要遇到带 `m_body` 的 group，就把它当成当前 body 起点
- 之后直到下一个 `m_body` 出现前，所有 group 都属于这个 body

### 2.2 按 body 把 group 分段

`src/document/document.cpp`

```cpp
std::vector<Document::BodyGroups> Document::get_groups_by_body() const
{
    std::vector<Document::BodyGroups> r;
    for (auto group : get_groups_sorted()) {
        if (group->m_body) {
            r.emplace_back(group->m_body.value());
        }
        assert(r.size());
        r.back().groups.push_back(group);
    }

    return r;
}
```

意思就是：

- 每遇到一个 `m_body`，就开始一个新的 body 分组
- 后续 group 都挂到这个 body 下

所以 `WITH_BODY` 的真实含义不是“创建实体”，而是：

- 在 group 历史链上插入一个新的 body 分段点


## 3. 它影响哪些内容

`WITH_BODY` 主要影响四件事。

### 3.1 影响 group 属于哪个 body

这是最根本的。

因为 body 是按 group 顺序分段出来的，所以 `WITH_BODY` 会改变：

- 当前新 group
- 以及它后面的连续 group

归属于哪个 body。


### 3.2 影响 solid model 的累计边界

solid model 的“前序实体”查找只在同一个 body 内进行。

`src/document/solid_model/solid_model.cpp`

```cpp
auto this_body = &group.find_body(doc).body;
...
auto body = &gr->find_body(doc).body;
if (body != this_body)
    continue;
```

这表示：

- body 不同，就不会沿着前一个 body 的实体链继续累积

所以 `WITH_BODY` 的效果是：

- 从这个 group 开始，solid model 累积链被切开
- 新 body 有自己的独立实体链


### 3.3 影响渲染和可见性控制

Renderer 是按 body 来绘制 solid model 的。

`src/render/renderer.cpp`

```cpp
auto groups_by_body = doc.get_groups_by_body();
for (auto body_groups : groups_by_body) {
    if (!m_doc_view->body_solid_model_is_visible(body_groups.get_group().m_uuid))
        continue;
    ...
}
```

所以 body 是这些行为的粒度：

- body 可见/隐藏
- body solid model 可见/隐藏
- body 颜色

如果新 group 用了 `WITH_BODY`，它就会成为一个新的独立显示单元。


### 3.4 影响导出组织

导出也是按 body 来的。

`src/editor/editor_export.cpp`

```cpp
auto groups_by_body = doc_info.get_document().get_groups_by_body();
for (auto body_groups : groups_by_body) {
    ...
    last_solid_model->add_to_step_exporter(exporter, body_groups.body.m_name.c_str());
}
```

所以 `WITH_BODY` 也会影响：

- 导出时有几个 body
- 每个 body 的名字和分组方式


## 4. 它不是什么

这里很容易误解，所以单独说一下。

`WITH_BODY` 不等于：

- 创建一个 `TopoDS_Shape`
- 创建一个现成实体
- 立刻生成一个 OCC solid

真正几何实体还是由后续 group 的 `update_solid_model()` 生成，例如：

`src/document/group/group_extrude.cpp`

```cpp
void GroupExtrude::update_solid_model(const Document &doc)
{
    m_solid_model = SolidModel::create(doc, *this);
}
```

所以 `WITH_BODY` 只决定：

- 这条特征链属于哪个 body

不直接决定：

- 几何长什么样


## 5. 对应什么工程实际需求

`WITH_BODY` 对应的核心工程需求是：

- 一个文档里经常要同时建多个彼此独立的实体单元

最常见有这几类场景。

### 5.1 一个文档里同时建多个独立零件/实体

例如同一个文档里同时做：

- 底座
- 压块
- 定位销

它们位置相关，但不是同一个实体。

这时就需要：

- 一个底座 body
- 一个压块 body
- 一个定位销 body


### 5.2 做完一个实体后，再开始另一个独立实体

例如：

```text
Sketch -> Extrude -> Fillet
```

这一条已经是一个完整实体链。

现在想再做一个独立柱子，就不应该继续沿着原实体链往下做，而应该：

- 新开一个 body


### 5.3 按实体分别控制显示、颜色、导出

工程上经常需要：

- 单独隐藏某个实体
- 单独给某个实体上色
- 单独导出某个实体

在 `dune3d` 里，这些组织粒度就是 body。


### 5.4 为后续布尔/组合操作准备独立输入体

很多时候不是立刻把新做的实体并到原实体，而是希望：

- 两个实体先独立存在
- 后面再决定是否 union / difference / intersection

这也要求系统能区分多个 body。


## 6. 一个最简单的建模例子

假设要在一个文档里做：

- 一个底座
- 一个旁边独立的小柱子

### 6.1 不使用 `WITH_BODY`

group 顺序可能是：

```text
Reference   [body start]
Sketch 1
Extrude 1   -> 底座
Sketch 2
Extrude 2   -> 小柱子
```

这时：

- `Sketch 2 / Extrude 2` 仍然属于第一个 body
- 系统更倾向于把它理解成“在原实体链上继续加特征”


### 6.2 使用 `WITH_BODY`

group 顺序可能变成：

```text
Reference   [body start]
Sketch 1
Extrude 1   -> 底座

Sketch 2    [new body start]
Extrude 2   -> 小柱子
```

这时：

- `Sketch 2` 开始就是新的 body
- `Extrude 2` 属于新的实体链
- 底座和柱子变成两个独立 body

这会影响：

- solid model 累积
- 渲染分组
- body 可见性
- 导出组织


## 7. 它对应 FreeCAD 的什么概念

概念上最接近 FreeCAD 的：

- `Body`

尤其接近这种语义：

- 从这里开始一个新的独立实体特征链

但实现上比 FreeCAD 轻很多：

- `dune3d` 没有单独维护一个很重的 Body 对象体系
- 而是通过 `group.m_body` + group 顺序来切分 body

所以可以这样理解：

- `dune3d` 的 `WITH_BODY` 在工程语义上接近“新建一个 Body”
- 但代码实现只是给 group 历史链打一个 body 起点标记


## 8. 总结

`WITH_BODY` 的本质不是“创建几何”，而是：

- 在特征历史链中显式开始一个新的 body

它解决的是这样的工程需求：

- 一个文档里要有多个独立实体
- 这些实体要分别累积、分别显示、分别导出、分别管理

所以如果只记一句：

- `WITHOUT_BODY` 更像“继续在当前实体链上加特征”
- `WITH_BODY` 更像“从这里开始新建一个独立实体/body”
