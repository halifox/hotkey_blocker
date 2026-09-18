# 变更记录

本项目所有值得记录的变化都将记录在此文件中。

格式参考 [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)，版本号遵循 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [Unreleased]

暂无。

## [1.0.0] - 2026-09-18

首个正式版本。

### Added

- 支持按单个 `.exe` 文件或文件夹添加规则；文件夹规则会递归匹配其中的应用程序。
- 支持启用、停用和删除规则，并将规则保存到当前用户的配置文件中。
- 监控目标进程的启动和退出，自动处理规则创建后启动的目标进程，并显示规则、进程和拦截状态。
- 通过进程架构选择对应的 Hook 组件，支持 Windows x86 和 x64 目标；x64 安装包同时包含 x86 Hook 和注入辅助程序。
- 支持系统托盘运行、重新打开主窗口、退出程序，以及登录时自动启动。
- 增加 UTF-8 INI 配置存储和日志记录，日志支持当前文件及两个轮转文件。
- 增加 CMake Presets、PowerShell 构建脚本、x86/x64 CI 和 CTest 测试流程。
- 增加 NSIS 安装包、项目图标、开始菜单/桌面快捷方式，以及用户指南、许可证和第三方声明。
- 增加配置读写、快捷键注册拦截注入和 `BlockerService` 进程生命周期的自动化测试。

### Changed

- Windows 目标默认使用静态 MSVC 运行库，发布程序不要求目标机器另外安装 Visual C++ Redistributable。
- 规则状态文本始终显示，并增加 Windows Explorer 重启后重新创建托盘图标的处理。
- 安装包和文件版本信息统一使用构建时传入的语义化版本号。

### Fixed

- 修复托盘初始化失败时的状态处理。
- 修复卸载时未清理 `HotkeyBlocker` 开机启动注册表值的问题。
- 修复卸载时未删除 `%LOCALAPPDATA%\HotkeyBlocker` 配置和日志目录的问题。

### Security

- 发布包不使用 Authenticode 签名；安装包提供同名 SHA-256 校验文件，便于核对下载文件的完整性。
- 程序只拦截标准 `RegisterHotKey` 调用，不绕过 Windows 权限边界，也不处理低级键盘钩子或键盘驱动实现的快捷键。

### 已知限制

- 目标应用已经运行时，需要重启目标应用才能加载 Hook。
- 目标进程权限高于 Hotkey Blocker、属于受保护进程、启用反注入/动态代码限制，或被安全软件拦截时，注入可能失败。
- 当前发布版本提供 x86/x64 组件，不提供独立的 ARM64 Hook 组件。
