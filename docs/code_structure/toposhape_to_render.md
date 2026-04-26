# TopoDS_Shape -> 渲染链路（精简要害）

本文只讲一条主线：**OCC 的 `TopoDS_Shape` 如何变成屏幕像素**。

---

## 1) 类图（核心）

```text
SolidModel (抽象)
  - m_faces : face::Faces
  - m_edges : map<edge_id, polyline>
        ^
        |
SolidModelOcc (OCC 实现)
  - m_shape / m_shape_acc : TopoDS_Shape
  - triangulate() / find_edges()

Renderer (业务渲染编排)
  - render(doc, view, ...)
  - visit(Entity/Constraint)
        |
        | uses
        v
ICanvas (绘制抽象接口)
        ^
        | implements
Canvas (Gtk::GLArea + OpenGL 后端)
  - m_chunks : vector<CanvasChunk>
  - m_all_renderers : vector<BaseRenderer*>
  - on_render()/render_all()
        |
        +--> FaceRenderer / LineRenderer / IconRenderer / Glyph... (具体 OpenGL pass)
```

要点：
- OCC 只在 `SolidModelOcc`（和 STEP 导入）层出现。
- `Renderer` 与 OpenGL 解耦（只依赖 `ICanvas`）。
- 真正发 `glDraw*` 的是 `src/canvas/*_renderer.cpp`。

---

## 2) 调用关系（主链路）

```text
[A] 建模阶段（OCC）
SolidModel::create(...)
  -> SolidModelOcc::m_shape = OCC 造型结果
  -> SolidModelOcc::update_acc_finish(...)
      -> update_acc(...) 得到 m_shape_acc
      -> finish(...)
          -> triangulate()  // m_shape_acc -> m_faces
          -> find_edges()   // m_shape_acc -> m_edges

[B] 业务渲染阶段
Editor::canvas_update()
  -> Renderer::render(...)
      -> m_ca.add_face_group(solid_model->m_faces, ...)
      -> m_ca.draw_line/draw_icon/...
      -> m_ca.add_selectable(...)
  -> Canvas::request_push()

[C] GPU 执行阶段
Canvas::on_render()
  -> Canvas::render_all()
      -> *Renderer.push()   // CPU buffer -> GPU buffer
      -> *Renderer.render() // glDrawElements / glDrawArrays...
```

---

## 3) 关键实现点（代码定位）

- OCC 生成 shape（示例：拉伸）  
  `src/document/solid_model/solid_model_extrude.cpp`
- 累计/布尔与收口  
  `src/document/solid_model/solid_model_occ.cpp` `update_acc_finish()` / `finish()`
- 三角化（OCC -> `face::Faces`）  
  `src/document/solid_model/solid_model_occ.cpp` `processFace()` / `triangulate()`
- 业务层喂给 Canvas  
  `src/render/renderer.cpp` `add_face_group(...)`
- OpenGL 绘制面  
  `src/canvas/face_renderer.cpp` `glDrawElementsBaseVertex(...)`
- 帧调度  
  `src/canvas/canvas.cpp` `on_render()` / `render_all()`

---

## 4) 设计结论（为什么这样做）

- **内核复用**：OCC 专注精确几何与布尔。
- **显示解耦**：渲染统一走 `face::Faces`，不让 UI/渲染层依赖 `TopoDS_*`。
- **交互可控**：拾取、高亮、约束图标、selection peeling 由自研渲染链控制。

一句话：**这套架构是 “OCC 做几何，项目做显示与交互” 的典型 CAD 分层。**

