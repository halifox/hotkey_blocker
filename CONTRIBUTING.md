# 贡献指南

感谢参与 Hotkey Blocker。本文档说明问题反馈、功能建议、源代码变更、构建测试和 Pull Request 的基本要求。

项目涉及 Windows 进程监控、架构匹配和 DLL 注入。提交变更前，请先阅读 [安全说明](SECURITY.md) 和 [贡献者公约](CODE_OF_CONDUCT.md)。

## 行为准则

参与项目时应遵守 [贡献者公约](CODE_OF_CONDUCT.md)，并避免在公开 Issue、Pull Request 或日志中提交凭据、私人数据、恶意 DLL 或其他敏感内容。

## 报告问题

提交 Bug 前，请搜索现有 Issue，确认问题尚未被报告。请使用 [Bug report 模板](.github/ISSUE_TEMPLATE/bug_report.md)，并提供：

- 实际行为和预期行为
- 最小复现步骤
- Hotkey Blocker 版本或 Git commit
- Windows 版本、目标进程架构和权限级别
- 脱敏后的错误信息和日志

请勿在公开 Issue 中发布安全漏洞的利用细节。安全问题应按照 [安全说明](SECURITY.md) 报告。

## 提出功能建议

请使用 [Feature request 模板](.github/ISSUE_TEMPLATE/feature_request.md)，说明使用场景、期望行为、兼容性影响和已考虑的替代方案。涉及权限、注入边界、配置格式或安装包的提议，应同时说明相应的安全影响。

## 准备环境

请按照 [构建、测试和打包](docs/BUILD.md) 准备 Visual Studio、Windows SDK、ATL、CMake、Ninja 和 PowerShell。贡献者至少应验证一个 x64 或 x86 构建；涉及注入器或 Hook 的变更应验证两个架构。

## 提交变更

1. 从最新默认分支创建短生命周期分支。
2. 保持每个提交范围清晰，提交信息准确描述实际变化。
3. 修改后用对应架构的 CMake preset 构建；涉及打包时按构建文档中的命令行步骤生成安装包。
4. 运行对应构建目录中的 CTest，并在 Pull Request 中记录结果。
5. 创建 Pull Request，填写变更内容、测试环境、已知限制和用户可见影响。

## 代码与文档规范

- 保持 C++20、Unicode 和现有 Windows API 风格。
- 不要在 UI 线程中加入长时间阻塞的进程扫描或注入操作。
- 所有跨进程句柄、线程和内存都必须有明确的生命周期。
- 错误路径应保留 Win32 错误码，并向用户提供可理解的说明。
- 不要记录密码、令牌或未经脱敏的私人路径。
- 新增文件使用 UTF-8；文档和用户可见文本保持清晰、一致并与实际行为相符。

## 构建与测试

提交前至少执行：

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --test-dir out/build/x64-release-vcpkg --output-on-failure

# 在 x86 Developer PowerShell 中执行
$env:VCPKG_ROOT = 'C:/dev/vcpkg'
cmake --preset x86-release
cmake --build --preset x86-release --parallel
ctest --test-dir out/build/x86-release-vcpkg --output-on-failure
```

涉及安装包时，按 [构建、测试和打包](docs/BUILD.md) 中的命令行步骤先生成 x86 运行组件，再生成 x64 安装包。

测试真实目标应用时，请使用可以随时重启的测试程序，不要对系统关键进程、反作弊进程或生产环境进程进行实验。

## Pull Request 提交清单

请使用 [Pull Request 模板](.github/PULL_REQUEST_TEMPLATE.md)，并确认：

- [ ] 说明用户可见变化和不兼容变化。
- [ ] 按需更新 README、用户指南、安全说明或变更记录。
- [ ] 验证相关 x86/x64 构建。
- [ ] 运行相关架构的 CTest，且结果通过。
- [ ] 未提交 `out/`、`bin/`、`cmake-build-*` 或个人配置。
- [ ] 未提交真实用户路径、日志或敏感数据。
- [ ] 新增第三方代码时补充来源和许可证声明。
