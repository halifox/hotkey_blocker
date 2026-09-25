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
- vcpkg

本地命令行构建前，先打开与目标架构匹配的 Visual Studio Developer PowerShell，并将 vcpkg 根目录设置为 `VCPKG_ROOT`。文中的打包和哈希示例使用 PowerShell。首次配置会按 `vcpkg.json` 下载并构建 Detours、WTL、CPR、libcurl、nlohmann/json 及其依赖。依赖采用静态 triplet，与本项目的静态 MSVC 运行库一致。

## 本地命令行构建

在 x64 Developer PowerShell 的仓库根目录执行。将 vcpkg 路径替换为本机实际路径：

```powershell
$env:VCPKG_ROOT = 'C:/dev/vcpkg'
cmake --preset x64-release
cmake --build --preset x64-release --parallel
```

不指定 `--target` 时会构建全部目标，包括主程序和 `hotkey_hook`，因此 x64 输出目录中会同时生成 `HotkeyBlocker.exe` 和 `HotkeyHook64.dll`。

Debug 使用 `x64-debug` preset：

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug --parallel
```

需要重新配置并清理目标文件时：

```powershell
cmake --fresh --preset x64-release
cmake --build --preset x64-release --clean-first --parallel
```

x86 构建需要在 x86 Developer PowerShell 中使用 `x86-release` 或 `x86-debug` preset，并在该 shell 中设置 `VCPKG_ROOT`。每个 preset 都有独立的构建目录。

## CMake Presets

`CMakePresets.json` 提供以下预设：

| 预设 | 目标 |
| --- | --- |
| `x64-release` | x64 Release |
| `x64-debug` | x64 Debug |
| `x86-release` | x86 Release |
| `x86-debug` | x86 Debug |

`CMakePresets.json` 中的 toolchain 使用当前 shell 的 `VCPKG_ROOT`。打开新的 Developer PowerShell 时，需要在该 shell 中再次设置它。x86 构建需要在 x86 MSVC 环境中执行，x64 构建需要在 x64 MSVC 环境中执行。

## 输出目录

- `build/<preset>-vcpkg/`：该 preset 的 CMake/Ninja 中间文件、manifest-mode 依赖和架构运行文件
- `build/x64-release-vcpkg/`：x64 主程序和 Hook DLL
- `build/x86-release-vcpkg/`：x86 主程序、Hook DLL 和注入辅助程序
- `build/packages/`：最终安装包和 SHA-256 校验文件

上述当前构建目录均被 `.gitignore` 忽略，不应提交到源码仓库。

## 运行库与测试

MSVC 目标默认使用静态运行库（Release 为 `/MT`，Debug 为 `/MTd`）。因此由本项目构建的主程序、Hook DLL 和辅助程序不要求目标机器另外安装 Visual C++ Redistributable；这不代表可以省略 Windows SDK、ATL 或构建机上的 MSVC 工具链。

测试目标默认启用。构建后运行：

```powershell
ctest --test-dir build/x64-release-vcpkg --output-on-failure
ctest --test-dir build/x86-release-vcpkg --output-on-failure
```

测试覆盖配置读写、快捷键策略判定与持久化、Hotkey 注册注入以及 BlockerService 的进程生命周期。仅构建产品目标时，可以在 CMake 配置阶段传入 `-DHKB_BUILD_TESTS=OFF`。

## 完整安装包

x64 安装包需要同时包含 x64 和 x86 组件，因为 64 位主程序可能需要处理 32 位目标进程。先安装 NSIS 并确保 `makensis.exe` 在 `PATH` 中。根目录的 `version.txt` 是默认版本来源，CMake 会据此生成程序版本头文件、Windows 版本资源和安装包文件名。发布 Tag 使用 `vMAJOR.MINOR.PATCH` 格式时，工作流会将去掉 `v` 的版本号传给构建。

从 Visual Studio Developer PowerShell 的仓库根目录运行下面这一条命令。它会依次构建 x86 Hook 和注入器、构建 x64 主程序和 Hook、运行 CPack，再把安装包与 SHA-256 文件放到 `build/packages/`。命令内部会分别启动 x86 和 x64 MSVC 环境，不需要手工切换终端。

```powershell
cmake -DVCPKG_ROOT=C:/dev/vcpkg -P cmake/package-x64.cmake
```

把 `C:/dev/vcpkg` 换成本机 vcpkg 根目录。命令完成后会根据 `version.txt` 生成 `build/packages/HotkeyBlocker-<版本号>-x64.exe` 和对应的 `.sha256` 校验文件。需要覆盖默认版本打包时，可传入 `-DHKB_VERSION_OVERRIDE="1.2.3"`。

安装器只为当前 Windows 用户安装，不请求管理员权限，默认目录为 `%LOCALAPPDATA%\Programs\Hotkey Blocker`；开始菜单快捷方式和 Windows 中的卸载项也只对当前用户生效。升级会在原安装目录覆盖文件，不会先运行旧版卸载程序，也不会删除用户配置。安装器和卸载程序会检查 Hotkey Blocker 程序文件及 Hook DLL 的占用情况。安装或卸载时如检测到占用，会列出应用名和 PID，提示用户自行关闭后重新运行相应程序，并取消本次操作。安装器和卸载程序不会替用户结束进程。

从旧版全局安装迁移到当前用户安装时，需先备份配置并使用旧版卸载程序移除 `Program Files` 中的版本；之后的升级可直接覆盖安装。

卸载时会删除程序文件、快捷方式和当前用户的登录启动项。配置及日志默认保留；只有在卸载器中勾选“删除配置文件和日志”时才删除已知配置、临时配置和日志文件。

安装包内包含：

- `HotkeyBlocker.exe`
- 当前架构的 Hook DLL
- x64 包中的 x86 Hook DLL 和 x86 注入辅助程序
- `README.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `docs/USER_GUIDE.md`
- `licenses/detours/copyright`
- `licenses/wtl/copyright`
- `licenses/cpr/copyright`
- `licenses/curl/copyright`
- `licenses/nlohmann-json/copyright`
- `licenses/zlib/copyright`

## 常见问题

### 找不到 CMake 或 Ninja

请使用 Visual Studio Developer PowerShell，或确认 CMake、Ninja、匹配架构的 MSVC 工具链和有效的 `VCPKG_ROOT` 均已配置。

### x64 构建时找不到 32 位组件

使用上面的 `cmake -P cmake/package-x64.cmake` 入口打包，它会先将 x86 Hook DLL 和注入辅助程序构建到 `build/x86-release-vcpkg/`，再构建 x64 并运行 CPack。若手动调用 x64 的 `package` 目标，则需要先自行生成这些 x86 组件。

### 注入失败

先确认目标进程和 Hook DLL 架构匹配，再检查目标程序是否以更高权限运行、是否受保护，以及日志中的错误码。注入失败不应通过降低系统安全策略来解决。
