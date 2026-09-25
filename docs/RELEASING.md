# GitHub 自动发布

## 发布流程

1. Pull Request 标题使用 Conventional Commits 格式，并以 squash merge 合并到 `master`。
2. 合并后，Release Please 根据提交生成或更新版本 PR。版本 PR 会更新 `version.txt`、`.release-please-manifest.json` 和 `CHANGELOG.md`。
3. 版本 PR 通过 x86、x64 构建检查和标题检查后自动合并。
4. Release Please 按新版本创建 `vX.Y.Z` 草稿 Release。自动发布工作流从该 Tag 启动现有 x86/x64 构建与测试流程。
5. 构建成功后，工作流上传 x64 安装包和 SHA-256 文件，并公开草稿 Release。构建失败时 Release 会保持草稿状态。

版本号按提交类型递增：

- `fix:`、`perf:` 和 `revert:` 递增 PATCH，例如 `1.0.4` → `1.0.5`。
- `feat:` 递增 MINOR，例如 `1.0.4` → `1.1.0`。
- `!` 或 `BREAKING CHANGE:` 递增 MAJOR。
- `docs:`、`style:`、`refactor:`、`test:`、`build:`、`ci:`、`chore:` 等类型单独出现时不会触发版本发布。

## GitHub 仓库设置

在工作流合并到 `master` 前完成以下设置：

1. 创建仅授权此仓库的 fine-grained token，并授予 `Contents: Read and write`、`Issues: Read and write` 和 `Pull requests: Read and write`。将它保存为仓库 Actions secret `RELEASE_PLEASE_TOKEN`。不要把令牌写入仓库或提交记录。
2. 在仓库设置中启用 Allow auto-merge。
3. 为 `master` 保留分支保护：要求 Pull Request，并将 `Build x64`、`Build x86`、`Validate PR title` 设为必需检查。不要允许绕过这些检查后自动合并。
4. 启用 squash merge，并把 PR 标题设为 squash 提交消息。版本工具读取合并后的 Conventional Commit 消息来确定版本增量。

自动化从这些配置进入 `master` 后开始。初始版本以当前变更记录的 `1.0.4` 为基准，已排除基准提交之前的历史记录。
