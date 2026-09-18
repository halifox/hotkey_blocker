# 贡献指南

感谢参与 Hotkey Blocker。项目涉及 Windows 进程监控、架构匹配和 DLL 注入，提交补丁前请先阅读 [安全说明](SECURITY.md)。

## 开发环境

请按照 [docs/BUILD.md](docs/BUILD.md) 安装 Visual Studio、Windows SDK、ATL、CMake、Ninja 和 PowerShell。贡献者应至少验证一个 x64 或 x86 构建；涉及注入器或 Hook 的变更应验证两个架构。

## 工作流程

1. 先搜索现有 Issue，避免重复工作。
2. 对行为变化先提交 Issue，说明动机、影响范围和兼容性要求。
3. 从最新 `master` 创建短生命周期分支，例如 `feature/rule-export` 或 `fix/injection-timeout`。
4. 保持提交小而聚焦，提交信息应说明实际变化，不要使用无意义的标题。
5. 修改后运行对应架构的构建；涉及打包时运行 `-Package`。
6. 运行对应构建目录中的 CTest，并在 Pull Request 中填写测试结果。
7. 创建 Pull Request，填写变更、测试环境、已知限制和用户可见影响。

## 代码要求

- 保持 C++20、Unicode 和现有 Windows API 风格。
- 不要在 UI 线程中加入长时间阻塞的进程扫描或注入操作。
- 所有跨进程句柄、线程和内存都必须有明确的生命周期。
- 错误路径应保留 Win32 错误码，并给用户可理解的说明。
- 不要记录密码、令牌或未经脱敏的私人路径。
- 新增文件应使用 UTF-8；文档和用户可见文本保持清晰一致。

## 测试和验证

当前项目重点是 Windows 集成验证。提交前至少执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
.\scripts\build.ps1 -Architecture x86 -Configuration Release
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir out/build/x64-release --output-on-failure
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir out/build/x86-release --output-on-failure
```

涉及安装包时再执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

测试真实目标应用时，请使用可随时重启的测试程序，不要对系统关键进程、反作弊进程或生产环境进程进行实验。

## Pull Request 清单

- [ ] 说明了用户可见变化和不兼容变化。
- [ ] 更新了 README、用户指南或安全文档（如适用）。
- [ ] 验证了相关 x86/x64 构建。
- [ ] 运行了相关架构的 CTest，且结果通过。
- [ ] 没有提交 `out/`、`bin/`、`cmake-build-*` 或个人配置。
- [ ] 没有提交真实用户路径、日志或敏感数据。
- [ ] 新增第三方代码时补充了来源和许可证声明。
