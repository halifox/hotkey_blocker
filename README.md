<p align="center">
  <img src="assets/HotkeyBlocker.png" alt="Hotkey Blocker" width="160">
</p>

<h1 align="center">Hotkey Blocker</h1>

<p align="center">把全局快捷键的控制权还给你。</p>

<p align="center">
  <a href="https://github.com/halifox/hotkey_blocker/actions/workflows/ci.yml"><img src="https://github.com/halifox/hotkey_blocker/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4?logo=windows&logoColor=white" alt="Platform: Windows 10/11">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/architecture-x86%20%7C%20x64-555555" alt="Architecture: x86 and x64">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-2ea44a" alt="License: MIT"></a>
</p>

<p align="center">
  <a href="#安装">安装</a> ·
  <a href="#功能">功能</a> ·
  <a href="#支持范围与限制">支持边界</a> ·
  <a href="#从源码构建">从源码构建</a> ·
  <a href="#工作原理">工作原理</a> ·
  <a href="#常见问题">常见问题</a>
</p>

Hotkey Blocker 是一个 Windows 桌面工具，用于阻止选定应用通过标准 `RegisterHotKey` API 注册全局快捷键。它按 EXE 或文件夹建立规则，自动匹配目标进程架构并加载对应的 Hook DLL，让你不必修改目标应用，也不必依赖目标应用是否提供快捷键设置。

主程序使用原生 Win32/WTL 界面，常驻系统托盘，配置和日志保存在当前用户的本地应用数据目录中。

> [!WARNING]
> **安全软件误报提醒**
>
> 本项目会向目标进程加载本地 DLL，并在目标进程中 Hook `RegisterHotKey`。这类行为与部分恶意软件的行为特征相似，杀毒软件、Windows Defender 或 SmartScreen 可能将程序或 Hook DLL 报告为风险程序。
>
> 只从可信来源获取构建产物。确认来源和行为后，再按需配置安全软件例外；不要为来源不明的程序直接添加信任，也不要用降低系统安全策略的方式解决注入失败。

## 为什么选择 Hotkey Blocker

有些应用会注册全局快捷键，却没有提供关闭或修改入口。Hotkey Blocker 只针对你选定的进程，在进程内部按配置拒绝标准 `RegisterHotKey` 调用；其他应用和普通键盘输入不受此规则直接影响。

| 需求 | Hotkey Blocker | 键盘重映射工具 | 应用内快捷键设置 |
| --- | --- | --- | --- |
| 不修改目标应用源码 | 支持 | 支持 | 不适用 |
| 按 EXE 或文件夹选择目标 | 支持 | 取决于工具 | 取决于应用 |
| 按应用配置快捷键策略 | 支持 | 取决于工具 | 取决于应用 |
| 拦截标准 `RegisterHotKey` | 支持 | 取决于工具 | 取决于应用 |
| 改变普通键盘输入 | 不支持 | 支持 | 取决于应用 |
| 处理低级键盘钩子、驱动或自定义快捷键 | 不支持 | 取决于工具 | 取决于应用 |

## 功能

### 按 EXE 或文件夹建立规则

可以添加单个 `.exe` 文件，也可以添加文件夹。文件夹规则会递归匹配其中的 EXE，适合统一处理一组工具或同一套软件的多个组件。

### 为每个应用配置快捷键策略

每个应用规则可以选择以下模式：

- **拦截全部**：拒绝该进程后续所有标准 `RegisterHotKey` 注册请求。
- **黑名单**：拒绝列表中的组合键，放行未列出的快捷键。
- **白名单**：只允许列表中的组合键，拒绝未列出的快捷键。

黑名单或白名单可以添加固定组合键，例如 `Ctrl+Alt+A`。快捷键按 Ctrl、Alt、Shift、Win 修饰键和虚拟键匹配；`MOD_NOREPEAT` 不参与匹配。

黑名单为空时表示放行全部快捷键；白名单为空时表示拦截全部快捷键。修改策略不会撤销目标应用已经成功注册的快捷键，因此修改后需要重启目标应用。

### 自动匹配 x86 / x64

程序根据目标进程架构选择对应的 Hook 组件。x64 安装包同时包含 x64 和 x86 运行组件，以便处理两种架构的目标进程；项目当前没有独立的 ARM64 Hook 组件。

### 监控进程并显示实际状态

规则列表会显示目标路径、启用状态、快捷键策略和拦截状态，包括等待启动、正在启用、拦截已生效、部分拦截生效、需要重启、启用失败、路径不存在和进程监控不可用等状态。

### 系统托盘与登录时启动

程序可以隐藏到系统托盘。托盘菜单提供“显示窗口”和“退出”；“登录时启动”使用当前用户的启动项，不安装 Windows 服务，也不要求通过服务常驻。

### 原生 Windows 界面

主窗口使用 Win32/WTL 和系统文件选择器，规则列表显示目标程序图标、路径、启用状态、快捷键策略及运行状态，便于直接定位问题。

### 本地配置与诊断日志

规则写入 UTF-8 INI 配置文件，进程发现、架构匹配、注入结果和错误信息写入本地日志，便于判断是路径、权限、架构还是安全软件导致的问题。

## 支持范围与限制

| 场景 | 支持情况 |
| --- | --- |
| Windows 10 / Windows 11 | 支持 |
| x86 / x64 目标进程 | 支持 |
| ARM64 专用 Hook | 当前未提供 |
| 标准 `RegisterHotKey` | 支持 |
| 按应用配置全部、黑名单和白名单策略 | 支持 |
| 低级键盘钩子、键盘驱动、自定义窗口消息快捷键 | 不保证支持 |
| 目标进程已在规则生效前运行 | 需要重启目标应用 |
| 目标进程以更高权限运行 | 可能无法打开或注入，不会绕过权限边界 |
| 受保护进程、反注入策略、动态代码限制 | 可能被系统或目标程序拒绝 |

启用或停用规则只影响后续注入以及之后启动的目标进程。已经加载 Hook 的进程不会被强制卸载 DLL，因此停用后通常仍需要重启目标应用才能完全恢复。

快捷键策略变更会标记已经运行的目标进程为需要重启。程序只拦截策略生效后发生的标准 `RegisterHotKey` 注册，不能事后撤销目标应用已经成功注册的快捷键。

## 安装

### 从发布版本安装

从 [GitHub Releases](https://github.com/halifox/hotkey_blocker/releases/latest) 下载安装包。x64 安装包包含主程序、x64 Hook，以及处理 32 位目标进程所需的 x86 Hook 和注入辅助程序。

安装并启动后：

1. 点击“添加 EXE”选择要限制的应用，或点击“添加文件夹”。
2. 确认规则处于启用状态。
3. 选中规则后点击“配置”，选择拦截全部、黑名单或白名单策略，并按需添加快捷键。
4. 如果目标应用已经运行，请完全退出并重新启动它。
5. 当状态显示“拦截已生效”后，该进程后续的标准全局快捷键注册会按当前策略处理。

## 从源码构建

### 构建环境

- Windows 10/11
- Visual Studio 的 Desktop development with C++ 工作负载
- MSVC x86/x64 编译工具、Windows SDK 和 ATL
- CMake 3.25 或更高版本
- Ninja
- PowerShell 5.1 或更高版本

### 构建

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
```

常用构建命令：

```powershell
# x86 发布版
.\scripts\build.ps1 -Architecture x86 -Configuration Release

# x64 调试版
.\scripts\build.ps1 -Architecture x64 -Configuration Debug

# 构建 x64 安装包，同时准备 x86 运行组件
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

也可以在已初始化的 Visual Studio Developer PowerShell 中使用 CMake Preset：

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

构建测试：

```powershell
ctest --test-dir out/build/x64-release --output-on-failure
```

主要产物：

| 路径 | 内容 |
| --- | --- |
| `out/build/<preset>/` | CMake/Ninja 中间文件 |
| `out/bin/x64-release/` | x64 主程序和 Hook DLL |
| `out/bin/x86-release/` | x86 主程序、Hook DLL 和注入辅助程序 |
| `out/packages/` | NSIS 安装包及 SHA-256 校验文件 |

完整的构建、测试、打包和 CI 说明见 [构建、测试和打包](docs/BUILD.md)。

## 运行

源码构建完成后，可以直接运行主程序：

```powershell
.\out\bin\x64-release\HotkeyBlocker.exe
```

程序启动后会显示主窗口并创建托盘图标。关闭主窗口只会将其隐藏；需要结束进程时，请使用托盘菜单中的“退出”。登录时启动会以后台模式启动程序。

## 操作入口

| 操作 | 入口 | 说明 |
| --- | --- | --- |
| 添加 EXE | 主窗口 → 添加 EXE | 为单个应用建立规则 |
| 添加文件夹 | 主窗口 → 添加文件夹 | 递归匹配文件夹内的 EXE |
| 配置快捷键 | 选中规则 → 配置 | 设置全部、黑名单或白名单策略 |
| 删除规则 | 选中规则 → 删除 | 删除规则，不会终止目标进程 |
| 启用/停用 | 选中规则 → 启用/停用 | 停用后已运行进程仍需重启 |
| 登录时启动 | 主窗口复选框 | 写入当前用户启动项 |
| 显示窗口 | 托盘菜单 → 显示窗口 | 从托盘恢复主窗口 |
| 退出 | 托盘菜单 → 退出 | 结束程序并移除托盘图标 |

## 工作原理

```text
                         HotkeyBlocker.exe
                    ┌────────────┼────────────┐
                    │            │            │
             RuleManager   ProcessMonitor   Tray / UI
             ConfigStore   BlockerService   StartupManager
                    │            │
                    └──────┬─────┘
                           ▼
                    HotkeyPolicyRegistry
                           │
                    按 PID 发布快捷键策略
                           │
                    ArchitectureDetector
                           │
             ┌─────────────┴─────────────┐
             │                           │
       x64 target process           x86 target process
             │                           │
      HotkeyHook64.dll       HotkeyBlockerInjector32.exe
                                         │
                                  HotkeyHook32.dll
             └─────────────┬─────────────┘
                           ▼
             Hook user32!RegisterHotKey
                           │
                           ▼
                 按应用策略返回成功或失败
```

工作流程：

- `ProcessMonitor` 发现匹配规则的目标进程，并记录 PID、架构和处理状态。
- `BlockerService` 根据目标进程匹配到的应用规则发布对应快捷键策略。
- `Injector` 根据目标进程架构选择 `HotkeyHook64.dll` 或 `HotkeyHook32.dll`；x64 主程序处理 32 位目标时使用 x86 注入辅助程序。
- Hook DLL 使用 Detours 拦截目标进程内部的 `user32!RegisterHotKey`，根据该进程的策略决定拒绝或调用原始 API。
- 规则、进程状态和错误信息通过主窗口及本地日志反馈给用户。

## 配置与数据

Hotkey Blocker 使用当前用户的本地应用数据目录：

| 路径 | 用途 |
| --- | --- |
| `%LOCALAPPDATA%\HotkeyBlocker\config.ini` | 规则路径、规则类型、启用状态和快捷键策略 |
| `%LOCALAPPDATA%\HotkeyBlocker\logs\hkb.log` | 进程监控、架构检测、注入结果和错误日志 |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\HotkeyBlocker` | 可选的登录时启动项 |

配置文件使用 UTF-8 INI 格式。当前配置版本为 2；旧版本规则会默认使用“拦截全部”策略。每个应用规则的快捷键策略使用 `HotkeyMode` 和编号的 `Hotkey.N` 项保存。

修改配置文件前建议先退出程序并备份。要恢复全部规则，可以退出 Hotkey Blocker 后备份并删除 `config.ini`；已经注入到目标进程中的 Hook 仍需通过重启目标应用来解除。

## 隐私与安全

- 配置和日志保存在本机当前用户目录，不需要云端账户。
- 日志可能包含目标程序路径、PID 和错误码。提交 Issue 或安全报告前，请脱敏用户名、项目路径和其他本地信息。
- 程序只针对规则匹配的目标进程执行本地 DLL 注入，不会绕过 Windows 权限边界。
- 快捷键策略通过本地进程级共享内存传递给对应 Hook DLL，不包含凭据或网络数据。
- 本项目不保证能够处理低级键盘钩子、驱动或目标程序自定义的快捷键实现。
- 详细安全说明见 [SECURITY.md](SECURITY.md)。

## 当前状态

| # | 能力 | 状态 |
| --- | --- | --- |
| 1 | Win32/WTL 主窗口与系统托盘 | 已完成 |
| 2 | 单 EXE 和文件夹递归规则 | 已完成 |
| 3 | x86/x64 Hook 与注入组件 | 已完成 |
| 4 | 进程监控与拦截状态展示 | 已完成 |
| 5 | 每个应用的全部、黑名单和白名单策略 | 已完成 |
| 6 | 本地配置、日志和登录时启动 | 已完成 |
| 7 | x86/x64 构建、测试和 NSIS 打包 | 已完成 |

相关设计和操作说明位于 [`docs/`](docs/)、[用户指南](docs/USER_GUIDE.md) 和 [构建说明](docs/BUILD.md)。

## 常见问题

### 为什么规则显示“重启程序后生效”？

目标进程在规则启用或快捷键策略修改前已经运行。程序不会强制卸载目标进程中的 DLL，也不会撤销目标应用已注册的快捷键，因此请完全退出并重新启动该应用。

### 黑名单和白名单有什么区别？

黑名单只拒绝列出的快捷键，其他快捷键放行；白名单只允许列出的快捷键，其他快捷键拒绝。黑名单为空表示放行全部，白名单为空表示拦截全部。

### 为什么目标程序仍然可以使用快捷键？

确认规则已启用，并重启目标应用。如果仍然有效，目标程序可能没有使用标准 `RegisterHotKey`，而是使用低级键盘钩子、驱动或自定义实现；也可能受权限、反注入策略或安全软件限制影响。

### 为什么显示“启用拦截失败”？

先查看 `%LOCALAPPDATA%\HotkeyBlocker\logs\hkb.log`，检查目标进程架构、权限、Hook DLL、快捷键策略共享内存和 x86 辅助程序是否完整。不要通过关闭系统保护或降低权限边界来解决问题。

### x64 安装包为什么还需要 x86 组件？

64 位主程序不能直接把 64 位 DLL 注入 32 位目标进程，因此 x64 安装包同时携带 x86 Hook DLL 和 `HotkeyBlockerInjector32.exe`。

### 如何清空配置并重新开始？

退出程序，备份后删除 `%LOCALAPPDATA%\HotkeyBlocker\config.ini`，再次启动时会创建空配置。已经运行的目标进程仍需要重启。

## 参与贡献

欢迎提交 Issue 和 Pull Request。提交修改前请阅读 [贡献指南](CONTRIBUTING.md)，并至少完成一次目标架构的 Release 构建和测试：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
ctest --test-dir out/build/x64-release --output-on-failure
```

涉及 Hook、注入、权限或安全边界的修改，请同时更新 [用户指南](docs/USER_GUIDE.md)、[安全说明](SECURITY.md) 或 [构建说明](docs/BUILD.md) 中对应的行为描述。

## 许可证

Hotkey Blocker 自有代码以 [MIT License](LICENSE) 发布。WTL、Detours 等第三方依赖的许可证和版权声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
