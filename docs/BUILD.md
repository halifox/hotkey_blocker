# 构建、测试和打包

本文档说明 Hotkey Blocker 在 Windows 上的本地构建、测试和安装包生成流程。

## 构建环境

推荐使用 Windows 10/11 和 64 位主机。项目源码提供 Windows x86/x64 目标，当前没有 ARM64 专用 Hook 组件。

需要安装：

- Visual Studio 的 Desktop development with C++ 工作负载
- MSVC x86/x64 编译工具
- Windows SDK
- ATL（WTL 依赖 ATL 头文件）
- CMake 3.25 或更高版本
- Ninja
- PowerShell 5.1 或更高版本

构建脚本通过 `vswhere.exe` 查找 Visual Studio，并调用对应的 `VsDevCmd.bat` 初始化 MSVC 环境。手工执行 CMake 命令时，请先进入 Visual Studio Developer PowerShell，或执行相应的 `VsDevCmd.bat`。

## 推荐构建方式

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
```

常用参数：

```powershell
# x86 Release
.\scripts\build.ps1 -Architecture x86 -Configuration Release

# x64 Debug
.\scripts\build.ps1 -Architecture x64 -Configuration Debug

# 删除本项目管理的 x64 Release 产物后重新构建
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Clean
```

`-Clean` 只删除脚本管理的 `out/build/<preset>` 和 `out/bin/<preset>` 目录，不会操作源码目录或其他构建目录。

## CMake Presets

`CMakePresets.json` 提供以下预设：

| 预设 | 目标 |
| --- | --- |
| `x64-release` | x64 Release |
| `x64-debug` | x64 Debug |
| `x86-release` | x86 Release |
| `x86-debug` | x86 Debug |

手工使用预设时，先初始化目标架构的 MSVC 环境：

```powershell
# 在 Developer PowerShell 或等价的 Visual Studio 环境中
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

x86 构建需要在 x86 MSVC 环境中执行。构建脚本会自动使用 `VsDevCmd.bat -arch=x86 -host_arch=x64` 或 `-arch=x64 -host_arch=x64`，适合重复构建和 CI 环境。

## 输出目录

- `out/build/<preset>/`：CMake/Ninja 中间文件
- `out/bin/x64-release/`：x64 可执行文件和 x64 Hook DLL
- `out/bin/x86-release/`：x86 可执行文件、Hook DLL 和 x86 注入辅助程序
- `out/packages/`：安装包和 SHA-256 校验文件

上述目录均被 `.gitignore` 忽略，不应提交到源码仓库。

## 运行库、测试与发布

MSVC 目标默认使用静态运行库（Release 为 `/MT`，Debug 为 `/MTd`）。因此由本项目构建的主程序、Hook DLL 和辅助程序不要求目标机器另外安装 Visual C++ Redistributable；这不代表可以省略 Windows SDK、ATL 或构建机上的 MSVC 工具链。

测试目标默认启用。构建后运行：

```powershell
ctest --test-dir out/build/x64-release --output-on-failure
ctest --test-dir out/build/x86-release --output-on-failure
```

测试覆盖配置读写、Hotkey 注册注入以及 BlockerService 的进程生命周期。仅构建产品目标时，可以在 CMake 配置阶段传入 `-DHKB_BUILD_TESTS=OFF`。

本项目的本地构建和正式发布均不使用 Authenticode 签名。发布工作流不会读取证书，也不会对主程序、Hook DLL、x86 注入辅助程序或安装包执行签名。

Windows 可能对从互联网下载的未签名程序显示未知发布者或 SmartScreen 警告，这是本项目发布策略的预期行为。不要要求用户关闭系统保护；发布页面应同时提供 SHA-256 校验文件，供用户核对下载文件的完整性。

GitHub Release 工作流不需要任何证书或签名相关的 Repository Secret。使用 `-Package` 时，脚本会在 `out/packages/` 生成未签名的安装包和同名的 SHA-256 校验文件。

## 完整安装包

x64 安装包需要同时包含 x64 和 x86 组件，因为 64 位主程序可能需要处理 32 位目标进程。推荐使用：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package
```

发布 Tag 使用 `vMAJOR.MINOR.PATCH` 格式时，可以显式传入版本，使 CMake、Windows 文件属性、安装包文件名和 Release 保持一致：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Package -Version 1.0.0
```

脚本会按以下顺序工作：

1. 构建 x86 Release 组件。
2. 构建 x64 Release 主程序和 Hook。
3. 使用 CPack 生成 NSIS 安装程序。
4. 复制安装包到 `out/packages/`。
5. 生成同名 `.sha256` 校验文件。

卸载程序会先结束正在运行的主程序，再删除 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\HotkeyBlocker`，并同步删除 `%LOCALAPPDATA%\HotkeyBlocker` 配置和日志目录，避免留下失效的开机启动项或用户数据。

安装包内包含：

- `HotkeyBlocker.exe`
- 当前架构的 Hook DLL
- x64 包中的 x86 Hook DLL 和 x86 注入辅助程序
- `README.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `docs/USER_GUIDE.md`
- `licenses/detours/LICENSE.md`

## CI

`.github/workflows/ci.yml` 会在 Windows runner 上分别构建 x86 和 x64 Release，并运行 CTest。`.github/workflows/release.yml` 会在推送 `v*` Tag 时构建 x64 安装包并生成 SHA-256 文件。

项目没有将 Visual Studio 编译器提交到仓库，因此不同 Visual Studio 版本不保证产生逐字节相同的二进制。若需要长期可复现的发布结果，应固定 GitHub runner、Visual Studio 工具链版本，并保存发布构建日志。

## 常见问题

### 找不到 CMake 或 Ninja

请使用 Visual Studio Developer PowerShell，或确认 CMake/Ninja 已加入 PATH。`build.ps1` 还要求系统能找到 `vswhere.exe`。

### x64 构建时找不到 32 位组件

不要只手工构建 x64。使用 `-Package`，脚本会先构建 x86 组件并将其放入 x64 安装包。

### 注入失败

先确认目标进程和 Hook DLL 架构匹配，再检查目标程序是否以更高权限运行、是否受保护，以及日志中的错误码。注入失败不应通过降低系统安全策略来解决。
