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
    if (left.version != right.version || left.autoStart != right.autoStart ||
        left.apps.size() != right.apps.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.apps.size(); ++index) {
        if (left.apps[index].path != right.apps[index].path ||
            left.apps[index].enabled != right.apps[index].enabled ||
            left.apps[index].recursive != right.apps[index].recursive) {
            return false;
        }
    }
    return true;
}

}  // namespace

int wmain() {
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

    AppConfig expected{
        2,
        true,
        {{L"C:\\程序\\示例.exe", true}, {L"D:\\工具\\禁用.exe", false}},
    };
    AppRule folderRule;
    folderRule.path = temporaryDirectory;
    folderRule.displayName = L"测试文件夹";
    folderRule.recursive = true;
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

    bool passed = Check(SameConfig(expected, actual), L"INI 内容往返一致");
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
    passed = Check(manager.Add(modulePathBuffer), L"添加规则并立即保存") && passed;
    passed = Check(!manager.Add(modulePathBuffer), L"拒绝重复规则") && passed;
    passed = Check(manager.SetEnabled(modulePathBuffer, false), L"保存停用状态") && passed;

    RuleManager reloaded{ConfigStore(managerConfigPath)};
    passed = Check(reloaded.Load(), L"重新加载规则") && passed;
    passed = Check(reloaded.Rules().size() == 1 && !reloaded.Rules().front().enabled,
                   L"规则状态持久化") &&
             passed;
    passed = Check(reloaded.Remove(modulePathBuffer), L"删除规则并立即保存") && passed;
    passed = Check(reloaded.Rules().empty(), L"删除后规则为空") && passed;

    DeleteFileW(managerConfigPath.c_str());
    DeleteFileW((managerConfigPath.wstring() + L".tmp").c_str());
    return passed ? 0 : 1;
}
