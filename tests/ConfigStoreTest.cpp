#include "ConfigStore.h"
#include "RuleManager.h"

#include <windows.h>

#include <filesystem>
#include <iostream>

namespace {

bool Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
    }
    return condition;
}

bool SameConfig(const AppConfig& left, const AppConfig& right) {
    if (left.version != right.version || left.apps.size() != right.apps.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.apps.size(); ++index) {
        if (left.apps[index].path != right.apps[index].path ||
            left.apps[index].enabled != right.apps[index].enabled ||
            left.apps[index].displayName != right.apps[index].displayName ||
            left.apps[index].kind != right.apps[index].kind ||
            !SameHotkeyPolicy(left.apps[index].hotkeyPolicy, right.apps[index].hotkeyPolicy)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int wmain() {
    bool passed = true;
    const uint32_t ctrlAlt = HotkeyPolicyConstants::kModifierControl |
                             HotkeyPolicyConstants::kModifierAlt;
    const HotkeySpec ctrlAltA{ctrlAlt, 'A'};
    const HotkeySpec ctrlAltANoRepeat{ctrlAlt | HotkeyPolicyConstants::kModifierNoRepeat, 'A'};
    const HotkeySpec ctrlAltB{ctrlAlt, 'B'};

    HotkeyPolicy blockAll;
    passed = Check(ShouldBlockHotkey(blockAll, ctrlAltA), L"拦截全部策略拦截快捷键") && passed;

    HotkeyPolicy blacklist;
    blacklist.mode = HotkeyMode::Blacklist;
    blacklist.hotkeys = {ctrlAltA};
    passed = Check(ShouldBlockHotkey(blacklist, ctrlAltANoRepeat),
                   L"黑名单忽略 MOD_NOREPEAT") &&
             passed;
    passed = Check(!ShouldBlockHotkey(blacklist, ctrlAltB), L"黑名单放行未列出的快捷键") &&
             passed;

    HotkeyPolicy whitelist;
    whitelist.mode = HotkeyMode::Whitelist;
    whitelist.hotkeys = {ctrlAltA};
    passed = Check(!ShouldBlockHotkey(whitelist, ctrlAltA), L"白名单放行列表中的快捷键") &&
             passed;
    passed = Check(ShouldBlockHotkey(whitelist, ctrlAltB), L"白名单拦截未列出的快捷键") &&
             passed;

    HotkeyPolicy emptyBlacklist;
    emptyBlacklist.mode = HotkeyMode::Blacklist;
    passed = Check(!ShouldBlockHotkey(emptyBlacklist, ctrlAltA),
                   L"空黑名单放行全部快捷键") &&
             passed;
    HotkeyPolicy emptyWhitelist;
    emptyWhitelist.mode = HotkeyMode::Whitelist;
    passed = Check(ShouldBlockHotkey(emptyWhitelist, ctrlAltA),
                   L"空白名单拦截全部快捷键") &&
             passed;

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (!Check(GetTempPathW(MAX_PATH, temporaryDirectory) != 0, L"获取临时目录")) {
        return 1;
    }

    wchar_t temporaryFile[MAX_PATH]{};
    if (!Check(GetTempFileNameW(temporaryDirectory, L"hkb", 0, temporaryFile) != 0,
               L"创建临时文件名")) {
        return 1;
    }

    const std::filesystem::path configPath(temporaryFile);
    DeleteFileW(configPath.c_str());
    DeleteFileW((configPath.wstring() + L".tmp").c_str());

    AppConfig expected;
    expected.apps = {{L"C:\\程序\\示例.exe", true}, {L"D:\\工具\\禁用.exe", false}};
    AppRule folderRule;
    folderRule.path = temporaryDirectory;
    folderRule.displayName = L"测试文件夹";
    folderRule.kind = RuleKind::Directory;
    folderRule.hotkeyPolicy.mode = HotkeyMode::Whitelist;
    folderRule.hotkeyPolicy.hotkeys = {
        {HotkeyPolicyConstants::kModifierControl | HotkeyPolicyConstants::kModifierAlt, 'A'},
        {HotkeyPolicyConstants::kModifierControl, 0x7b},
    };
    expected.apps.push_back(folderRule);

    ConfigStore store(configPath);
    std::wstring error;
    if (!Check(store.Save(expected, error), L"保存 UTF-8 INI")) {
        std::wcerr << error << L'\n';
        return 1;
    }

    AppConfig actual;
    if (!Check(store.Load(actual, error), L"重新加载 INI")) {
        std::wcerr << error << L'\n';
        DeleteFileW(configPath.c_str());
        return 1;
    }

    passed = Check(SameConfig(expected, actual), L"INI 内容往返一致") && passed;
    passed = Check(GetFileAttributesW((configPath.wstring() + L".tmp").c_str()) ==
                       INVALID_FILE_ATTRIBUTES,
                   L"临时文件已清理") &&
             passed;

    DeleteFileW(configPath.c_str());
    DeleteFileW((configPath.wstring() + L".tmp").c_str());

    wchar_t modulePathBuffer[MAX_PATH]{};
    if (!Check(GetModuleFileNameW(nullptr, modulePathBuffer, MAX_PATH) != 0,
               L"获取测试程序路径")) {
        return 1;
    }

    wchar_t managerTemporaryFile[MAX_PATH]{};
    if (!Check(GetTempFileNameW(temporaryDirectory, L"hkr", 0, managerTemporaryFile) != 0,
               L"创建规则测试文件名")) {
        return 1;
    }
    const std::filesystem::path managerConfigPath(managerTemporaryFile);
    DeleteFileW(managerConfigPath.c_str());
    DeleteFileW((managerConfigPath.wstring() + L".tmp").c_str());

    RuleManager manager{ConfigStore(managerConfigPath)};
    passed = Check(manager.Load(), L"加载空规则") && passed;
    AppRule executableRule;
    executableRule.path = modulePathBuffer;
    passed = Check(manager.AddRule(executableRule), L"添加规则并立即保存") && passed;
    passed = Check(!manager.AddRule(executableRule), L"拒绝重复规则") && passed;
    HotkeyPolicy policy;
    policy.mode = HotkeyMode::Blacklist;
    policy.hotkeys = {
        {HotkeyPolicyConstants::kModifierControl | HotkeyPolicyConstants::kModifierAlt, 'A'}};
    passed = Check(manager.SetRuleSettings(modulePathBuffer, false, policy), L"保存规则设置") &&
             passed;

    RuleManager reloaded{ConfigStore(managerConfigPath)};
    passed = Check(reloaded.Load(), L"重新加载规则") && passed;
    passed = Check(reloaded.Rules().size() == 1 && !reloaded.Rules().front().enabled,
                   L"规则状态持久化") &&
             passed;
    passed = Check(reloaded.Rules().front().hotkeyPolicy.mode == HotkeyMode::Blacklist &&
                       reloaded.Rules().front().hotkeyPolicy.hotkeys.size() == 1 &&
                       reloaded.Rules().front().hotkeyPolicy.hotkeys.front().virtualKey == 'A',
                   L"快捷键策略持久化") &&
              passed;
    passed = Check(reloaded.Remove(modulePathBuffer), L"删除规则并立即保存") && passed;
    passed = Check(reloaded.Rules().empty(), L"删除后规则为空") && passed;

    DeleteFileW(managerConfigPath.c_str());
    DeleteFileW((managerConfigPath.wstring() + L".tmp").c_str());
    return passed ? 0 : 1;
}
