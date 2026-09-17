#pragma once

#include "ConfigStore.h"

#include <string>
#include <vector>

namespace PathUtils {

std::wstring NormalizePath(const std::wstring& path);
bool SamePath(const std::wstring& left, const std::wstring& right);
bool IsPathUnderDirectory(const std::wstring& path, const std::wstring& directory);
int FindRuleIndex(const std::vector<AppRule>& rules, const std::wstring& path);

}  // namespace PathUtils
