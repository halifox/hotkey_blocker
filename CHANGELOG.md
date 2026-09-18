# 变更记录

本项目所有值得记录的变化都将记录在此文件中。

格式参考 [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)，版本号遵循 [Semantic Versioning](https://semver.org/spec/v2.0.0.html)。

## [Unreleased]

### Added

- 增加项目自定义图标，并用于窗口、系统托盘、安装包和快捷方式。
- 增加 CMake Presets、Windows 构建脚本、CI 和 Release 打包流程。
- 增加用户指南、安全说明、贡献指南和行为准则。

### Changed

- 使用静态 MSVC 运行库，减少安装包对外部 Visual C++ Redistributable 的依赖。
- 始终显示规则状态文本，并补充 Windows Explorer 重启后的托盘恢复处理。
- 安装包包含用户文档、项目许可证和第三方声明，并生成 SHA-256 校验文件。

### Fixed

- 修复托盘初始化失败后的状态处理。
- 修复卸载时未清理 `HotkeyBlocker` 开机启动注册表值的问题。
- 恢复配置、注入和 BlockerService 集成测试，并在 CI 中执行 CTest。

### Security

- Release 工作流要求使用 Authenticode 证书签名主程序、DLL、辅助程序和安装包。
