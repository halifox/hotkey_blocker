# 构建、测试和打包

## 构建环境

推荐使用 Windows 10/11 和 64 位主机。项目源码按 Windows x86/x64 目标构建，当前没有 ARM64 专用 Hook 组件。

需要安装：

- Visual Studio 的 Desktop development with C++ 工作负载
- MSVC x86/x64 编译工具
- Windows SDK
- ATL（WTL 依赖 ATL 头文件）
- CMake 3.25 或更高版本
- Ninja
- PowerShell 5.1 或更高版本

构建脚本通过 `vswhere.exe` 查找 Visual Studio，并调用对应的 `VsDevCmd.bat` 初始化 MSVC 环境。若使用手工命令，必须先进入 Visual Studio Developer PowerShell，或者执行相应的 `VsDevCmd.bat`。

## 推荐构建方式

在仓库根目录执行：

```powershell
.\scripts\build.ps1 -Architecture x64 -Configuration Release
```

可选参数：

```powershell
# x86 Release
.\scripts\build.ps1 -Architecture x86 -Configuration Release

# x64 Debug
.\scripts\build.ps1 -Architecture x64 -Configuration Debug

# 删除本项目 out/build 和 out/bin 中对应预设的产物后重新构建
.\scripts\build.ps1 -Architecture x64 -Configuration Release -Clean
```

脚本只删除它自己管理的 `out/build/<preset>` 和 `out/bin/<preset>` 目录，不会操作源码目录或其他构建目录。

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
# 在 Developer PowerShell 或等价的 VS 环境中
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

x86 构建需要在 x86 MSVC 环境中执行。构建脚本会自动使用 `VsDevCmd.bat -arch=x86 -host_arch=x64` 或 `-arch=x64 -host_arch=x64`，因此更适合重复构建和 CI。

## 输出目录

- `out/build/<preset>/`：CMake/Ninja 中间文件
- `out/bin/x64-release/`：x64 可执行文件和 x64 Hook DLL
- `out/bin/x86-release/`：x86 可执行文件、Hook DLL 和 x86 注入辅助程序
- `out/packages/`：安装包和 SHA-256 校验文件

这些目录都被 `.gitignore` 忽略，不应提交到源码仓库。

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

`.github/workflows/ci.yml` 会在 Windows runner 上分别构建 x86 和 x64 Release。`.github/workflows/release.yml` 会在推送 `v*` Tag 时构建 x64 安装包并生成 SHA-256 文件。

项目没有把 Visual Studio 编译器本身提交到仓库，因此不同 Visual Studio 版本不保证产生逐字节相同的二进制。若需要长期可复现的发布结果，应固定 GitHub runner、Visual Studio 工具链版本，并保存发布构建日志。

## 常见问题

### 找不到 CMake 或 Ninja

请使用 Visual Studio Developer PowerShell，或确认 CMake/Ninja 已加入 PATH。`build.ps1` 还要求系统能找到 `vswhere.exe`。

### x64 构建时找不到 32 位组件

不要只手工构建 x64。使用 `-Package`，脚本会先构建 x86 组件并将其放入 x64 安装包。

### 注入失败

先确认目标进程和 Hook DLL 架构匹配，再检查目标程序是否以更高权限运行、是否受保护，以及日志中的错误码。注入失败不应通过降低系统安全策略来解决。
