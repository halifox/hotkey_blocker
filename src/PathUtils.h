#pragma once

#include "ConfigStore.h"

#include <string>
#include <vector>

namespace PathUtils {

std::wstring NormalizePath(const std::wstring& path);
bool SamePath(const std::wstring& left, const std::wstring& right);
bool NormalizeRule(AppRule& rule, std::wstring& error);
bool NormalizeAndValidateRules(std::vector<AppRule>& rules, std::wstring& error);
bool ValidateRules(const std::vector<AppRule>& rules, std::wstring& error);
bool Matches(const AppRule& rule, const std::wstring& path);
bool Overlaps(const AppRule& left, const AppRule& right);

}  // namespace PathUtils
