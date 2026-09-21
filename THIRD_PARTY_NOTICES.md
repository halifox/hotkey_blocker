# 第三方依赖与许可证

Hotkey Blocker 的自有代码以仓库根目录的 [MIT License](LICENSE) 发布。下列依赖保留各自的版权和许可证，这些许可证不被项目根目录许可证替代。

## Microsoft Detours

- 来源：vcpkg `detours` port；版本由 `vcpkg.json` 中的 baseline 固定
- 版权所有：Microsoft Corporation
- 许可证：MIT License
- 安装包许可证文本：`licenses/detours/copyright`（来自 vcpkg 包）

## Windows Template Library（WTL）

- 来源：vcpkg `wtl` port；版本由 `vcpkg.json` 中的 baseline 固定
- 版权所有：Microsoft Corporation、WTL Team
- 许可证：Microsoft Public License（MS-PL）
- 许可证标识：`MS-PL`
- 安装包许可证文本：`licenses/wtl/copyright`（来自 vcpkg 包）

MS-PL 的标准文本可从 [Open Source Initiative](https://opensource.org/license/ms-pl-html) 获取。

## CPR、libcurl、nlohmann/json 与 zlib

主程序静态链接以下 vcpkg 依赖，版本由仓库根目录的 `vcpkg.json` 和其 baseline 固定：

- CPR：MIT License；安装包保留 `licenses/cpr/copyright`。
- libcurl：curl License；安装包保留 `licenses/curl/copyright`。
- nlohmann/json：MIT License；安装包保留 `licenses/nlohmann-json/copyright`。
- zlib：zlib License；作为 libcurl 的依赖，安装包保留 `licenses/zlib/copyright`。

上述许可证文件直接来自对应的 vcpkg 包。各项目的版权和许可证条款以安装包内副本为准。

## Windows SDK 与系统库

项目使用 Windows SDK、ATL、系统库以及 Visual C++ 运行时构建。上述组件由 Microsoft 单独授权，不作为本仓库的第三方源码重新授权。

## 分发要求

二进制安装包应保留安装目录中的 `LICENSE` 和 `THIRD_PARTY_NOTICES.md`，并在发布页面提供本项目源码或源码仓库链接。
