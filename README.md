# Hotkey Blocker

> 我讨厌各种流氓软件注册全局快捷键，导致我当前使用的软件快捷键失效。更让人恼火的是，这些设置项往往隐藏得很深，甚至根本没有设置项。

Hotkey Blocker 是一个 Windows 工具，用于阻止指定应用通过标准 `RegisterHotKey` API 注册全局快捷键。

它适合处理“某个应用占用了本应由当前应用使用的全局快捷键”这类问题。项目通过按目标进程架构加载对应的 Hook DLL 来完成拦截。

## 功能

- 添加单个 `.exe` 文件
- 添加文件夹并递归匹配其中的应用
- 自动处理之后启动的目标进程
- 启用、停用或删除规则
- 显示目标应用的运行、注入和拦截状态
- 系统托盘运行
- 登录时自动启动
- 同时提供 x86 和 x64 构建

## 支持范围和限制

- 当前发布目标是 Windows x86/x64；ARM64 尚未作为独立架构支持。
- 目标应用已经运行时，需要先重启目标应用才能加载 Hook。
- 目标应用以更高权限运行时，本程序可能无法打开或注入它；本程序不会绕过 Windows 权限边界。
- 只拦截标准 `RegisterHotKey` 调用，不保证拦截应用自定义的键盘驱动、低级键盘钩子或其他快捷键实现。
- 受保护进程、反作弊软件、动态代码策略或安全软件可能拒绝 DLL 注入。
- 本项目不是安全边界，也不是反恶意软件工具。注入行为可能触发杀毒软件或 SmartScreen 的误报。

## 快速构建

要求：带 Desktop C++ 工作负载的 Visual Studio、MSVC、Windows SDK、ATL、CMake 3.25 或更高版本、Ninja 和 PowerShell。完整说明见 [docs/BUILD.md](docs/BUILD.md)。

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
```

生成完整 x64 安装包（同时构建 x86 组件）：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

构建产物位于 `out/bin/`，安装包和 SHA-256 校验文件位于 `out/packages/`。

也可以在已经初始化的 Visual Studio Developer PowerShell 中直接使用预设：

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

构建预设会把不同架构和配置放入不同目录，避免 Debug/Release 或 x86/x64 产物相互覆盖。

## 使用文档

- [构建、测试和打包](docs/BUILD.md)
- [用户指南和故障排查](docs/USER_GUIDE.md)
- [安全说明和威胁模型](SECURITY.md)
- [贡献指南](CONTRIBUTING.md)
- [变更记录](CHANGELOG.md)

## 配置和日志

默认情况下，程序使用当前用户的本地应用数据目录：

- 配置：`%LOCALAPPDATA%\HotkeyBlocker\config.ini`
- 日志：`%LOCALAPPDATA%\HotkeyBlocker\logs\hkb.log`

日志可能包含目标进程路径和 PID。提交 Issue 前请检查并脱敏。

## 许可证

本项目自有代码以 [MIT License](LICENSE) 发布。依赖项的许可证和版权声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
