# Hotkey Blocker

Hotkey Blocker 是 Windows 桌面工具，用于阻止选定应用通过标准 `RegisterHotKey` API 注册全局快捷键，避免目标应用占用其他程序需要的快捷键。

程序根据目标进程的架构加载相应的 Hook DLL，在目标进程中拦截标准快捷键注册请求。它适用于目标应用未提供相关设置、但用户需要保留快捷键控制权的场景。

> [!WARNING]
> **安全软件误报提醒**
>
> 本项目使用 Windows API 对其他程序执行 DLL 注入和 API Hook，以拦截目标程序的全局快捷键注册请求。由于这类行为与部分恶意软件的行为特征相似，杀毒软件、Windows Defender 或 SmartScreen 可能会将本程序或 Hook DLL 报告为病毒/风险程序。
>
> 这可能属于误报；如果你确认程序来源可信、并已了解其注入行为，请在安全软件中将程序或相关文件加入信任/排除项后再运行。请勿对来源不明的构建产物直接添加信任。

## 主要功能

- 添加单个 `.exe` 文件规则
- 添加文件夹规则并递归匹配其中的应用程序
- 自动处理规则创建后启动的目标进程
- 启用、停用和删除规则
- 为每个应用配置“拦截全部、黑名单或白名单”快捷键策略
- 在黑名单/白名单中添加固定组合键，例如 `Ctrl+Alt+A`
- 显示规则、目标进程和拦截状态
- 在系统托盘中运行
- 支持登录时自动启动
- 提供 x86 和 x64 构建

## 支持范围与限制

- 发布版本支持 Windows x86/x64；项目未提供独立的 ARM64 Hook 组件。
- 目标应用已运行时，需要先重启该应用才能加载 Hook。
- 目标应用以更高权限运行时，程序可能无法打开或注入该进程；程序不会绕过 Windows 权限边界。
- 程序只拦截标准 `RegisterHotKey` 调用；黑名单和白名单按 Ctrl、Alt、Shift、Win 修饰键及虚拟键匹配，不保证处理低级键盘钩子、键盘驱动或其他自定义快捷键实现。
- 修改快捷键策略后，需要重启目标应用；已经注册的快捷键不会被事后撤销。
- 受保护进程、反作弊软件、动态代码策略和安全软件可能拒绝 DLL 注入。
- 本项目不是安全边界或反恶意软件工具。注入行为可能触发杀毒软件或 SmartScreen 的误报。

## 从源码构建

构建需要 Visual Studio 的 Desktop development with C++ 工作负载、MSVC、Windows SDK、ATL、CMake 3.25 或更高版本、Ninja 和 PowerShell。完整说明见 [构建、测试和打包](docs/BUILD.md)。

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
```

生成完整 x64 安装包（同时构建 x86 组件）：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

构建产物位于 `out/bin/`，安装包和 SHA-256 校验文件位于 `out/packages/`。

在已初始化的 Visual Studio Developer PowerShell 中，也可以直接使用 CMake 预设：

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

不同架构和配置使用独立输出目录，避免 Debug/Release 或 x86/x64 产物相互覆盖。

## 项目文档

- [构建、测试和打包](docs/BUILD.md)
- [用户指南和故障排查](docs/USER_GUIDE.md)
- [安全说明](SECURITY.md)
- [贡献指南](CONTRIBUTING.md)
- [行为准则](CODE_OF_CONDUCT.md)
- [变更记录](CHANGELOG.md)
- [第三方依赖与许可证](THIRD_PARTY_NOTICES.md)

## 配置文件与日志

默认情况下，程序使用当前用户的本地应用数据目录：

- 配置：`%LOCALAPPDATA%\HotkeyBlocker\config.ini`
- 日志：`%LOCALAPPDATA%\HotkeyBlocker\logs\hkb.log`

日志可能包含目标进程路径和 PID。提交 Issue 或安全报告前，请检查并脱敏相关内容。

## 许可证

本项目自有代码以 [MIT License](LICENSE) 发布。依赖项的许可证和版权声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
