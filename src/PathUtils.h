#pragma once

#include "ConfigStore.h"

#include <string>

namespace PathUtils {

std::wstring NormalizePath(const std::wstring& path);
bool SamePath(const std::wstring& left, const std::wstring& right);
bool IsPathUnderDirectory(const std::wstring& path, const std::wstring& directory);
bool NormalizeRule(AppRule& rule, std::wstring& error);
bool Matches(const AppRule& rule, const std::wstring& path);
bool Overlaps(const AppRule& left, const AppRule& right);

}  // namespace PathUtils
