# 变更记录

项目遵循“发布版本记录用户可见变化”的原则。未发布内容放在 `Unreleased` 下。

## Unreleased

### 修复

- 增加项目自定义图标，并用于窗口、托盘、安装包和快捷方式。
- 始终显示规则状态文本，补充托盘初始化失败和 Explorer 重启后的托盘恢复处理。
- 使用静态 MSVC 运行库，避免安装包依赖外部 Visual C++ Redistributable。
- 卸载时清理 `HotkeyBlocker` 开机启动注册表值。
- Release 工作流要求使用 Authenticode 证书签名主程序、DLL、辅助程序和安装包。
- 恢复配置、注入和 BlockerService 集成测试，并在 CI 中执行 CTest。

### 项目维护

- 增加项目许可证和第三方依赖声明。
- 增加 CMake Presets、Windows 构建脚本、CI 和 Release 打包流程。
- 增加用户指南、安全威胁模型、贡献指南和行为准则。
- 安装包包含用户文档、项目许可证和第三方声明，并生成 SHA-256 校验文件。
