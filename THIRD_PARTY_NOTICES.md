# Third-party notices

Hotkey Blocker 的自有代码以仓库根目录的 [MIT License](LICENSE) 发布。下面的依赖保留各自的版权和许可证；这些许可证不被项目根目录许可证替代。

## Microsoft Detours

- 位置：`third_party/detours/`
- 版本：以该目录中随源码分发的版本为准
- 版权所有：Microsoft Corporation
- 许可证：MIT License
- 源码许可证文本：[third_party/detours/LICENSE.md](third_party/detours/LICENSE.md)
- 安装包许可证文本：`licenses/detours/LICENSE.md`

Detours 的源文件和许可证头部均应随分发包保留。

## Windows Template Library (WTL)

- 位置：`third_party/wtl/`
- 版本：WTL 10.x（具体版本见各头文件头部声明）
- 版权所有：Microsoft Corporation, WTL Team
- 许可证：Microsoft Public License (MS-PL)
- 许可证标识：`MS-PL`

WTL 各头文件包含原始版权和许可证声明。本项目没有移除这些声明。MS-PL 的标准文本可从 [Open Source Initiative](https://opensource.org/license/ms-pl-html) 获取。

## Windows SDK 和系统库

项目使用 Windows SDK、ATL、系统库以及 Visual C++ 运行时来构建。这些组件由 Microsoft 单独授权，不作为本仓库的第三方源码重新授权。构建和运行要求请参见 [构建说明](docs/BUILD.md)。

## 分发要求

二进制安装包应保留安装目录中的 `LICENSE`、`THIRD_PARTY_NOTICES.md`，并在发布页面提供本项目源码或源码仓库链接。
