# 贡献指南

感谢参与 Hotkey Blocker。项目涉及 Windows 进程监控、架构匹配和 DLL 注入；提交变更前，请先阅读 [安全说明](SECURITY.md) 和 [行为准则](CODE_OF_CONDUCT.md)。

## 开始之前

请按照 [构建、测试和打包](docs/BUILD.md) 准备 Visual Studio、Windows SDK、ATL、CMake、Ninja 和 PowerShell。贡献者至少应验证一个 x64 或 x86 构建；涉及注入器或 Hook 的变更应验证两个架构。

提交新功能或行为变化前，建议先创建 Issue，说明使用场景、变更范围、兼容性影响和安全边界。提交前请搜索现有 Issue，避免重复工作。

## 提交变更

1. 从最新默认分支创建短生命周期分支。
2. 保持每个提交范围清晰，提交信息准确描述实际变化。
3. 修改后运行对应架构的构建；涉及打包时同时运行 `-Package`。
4. 运行对应构建目录中的 CTest，并在 Pull Request 中记录结果。
5. 创建 Pull Request，说明变更内容、测试环境、已知限制和用户可见影响。

## 代码与文档要求

- 保持 C++20、Unicode 和现有 Windows API 风格。
- 不要在 UI 线程中加入长时间阻塞的进程扫描或注入操作。
- 所有跨进程句柄、线程和内存都必须有明确的生命周期。
- 错误路径应保留 Win32 错误码，并向用户提供可理解的说明。
- 不要记录密码、令牌或未经脱敏的私人路径。
- 新增文件使用 UTF-8；文档和用户可见文本保持清晰、一致并与实际行为相符。

## 构建与测试

提交前至少执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
.\scripts\build.ps1 -Architecture x86 -Configuration Release
ctest --test-dir out/build/x64-release --output-on-failure
ctest --test-dir out/build/x86-release --output-on-failure
```

涉及安装包时再执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

测试真实目标应用时，请使用可以随时重启的测试程序，不要对系统关键进程、反作弊进程或生产环境进程进行实验。

## Pull Request 提交清单

- [ ] 说明用户可见变化和不兼容变化。
- [ ] 按需更新 README、用户指南、安全说明或变更记录。
- [ ] 验证相关 x86/x64 构建。
- [ ] 运行相关架构的 CTest，且结果通过。
- [ ] 未提交 `out/`、`bin/`、`cmake-build-*` 或个人配置。
- [ ] 未提交真实用户路径、日志或敏感数据。
- [ ] 新增第三方代码时补充来源和许可证声明。
