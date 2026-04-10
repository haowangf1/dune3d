# Repository Guidelines

## 项目结构与模块组织
- `src/` 是主要 C/C++ 代码目录。核心模块包括：`core/`（模型/内核逻辑）、`editor/` 与 `workspace/`（交互工具与状态）、`canvas/` 与 `render/`（视口与渲染）、`dialogs/` 与 `widgets/`（GTK 界面）。
- `src/python_module/` 用于构建可选 Python 扩展模块（`dune3d_py`）。
- `scripts/` 存放开发辅助脚本（尤其是代码风格检查）。
- `3rd_party/` 为第三方依赖代码；除依赖相关修复外，尽量不要在此目录做重构。
- `.github/workflows/` 定义 CI 的构建与风格检查标准，应与本地流程保持一致。

## 构建、测试与开发命令
- 初始化本地构建目录：`meson setup build`
- 构建主程序：`ninja -C build dune3d`（或 `ninja -C build all`）
- 构建 Python 模块：`ninja -C build dune3d_py.so`
- 按 CI 同步执行风格检查：`bash scripts/stylecheck.sh`
- 可选：使用 Nix 做一致性构建：`nix build .`

## 代码风格与命名约定
- 使用仓库内 `.clang-format` 配置；CI 要求 **clang-format 18**。
- 提交 C/C++ 代码前执行：`bash scripts/run_clangformat.sh`。
- 命名遵循现有约定：
  - 类型/类：`PascalCase`
  - 函数/变量/文件：`snake_case`
- 新增代码的 include 顺序与文件结构应尽量对齐相邻文件，避免引入新的个人风格。

## 测试指南
- 当前仓库没有完善的独立单元测试体系，主要依赖“可构建 + 冒烟验证”。
- 提交 PR 前至少完成：
  - 成功构建 `dune3d`；
  - 若修改 Python 绑定，构建 `dune3d_py.so` 并执行：
    - `python3 -c "import sys; sys.path.append('build'); import dune3d_py"`
- 涉及 UI 或交互行为修改时，请在 PR 描述中附上简短的人工验证说明。

## 提交与 Pull Request 规范
- Commit 标题保持简洁、祈使语气；常见格式为 `subsystem: action`，例如：`editor: add equivalent tool for context menu`。
- 每个提交聚焦单一目的；重构与行为变更尽量分开。
- PR 需包含：变更目的、用户可见影响、验证步骤；有对应 issue 时请关联。
- 若影响 UI 行为，请附截图或简短录屏。
- 提交前请阅读 `CONTRIBUTING.md` 并确保符合项目约定。
