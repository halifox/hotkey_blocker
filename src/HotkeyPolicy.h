#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace HotkeyPolicyConstants {

inline constexpr std::uint32_t kModifierAlt = 0x0001u;
inline constexpr std::uint32_t kModifierControl = 0x0002u;
inline constexpr std::uint32_t kModifierShift = 0x0004u;
inline constexpr std::uint32_t kModifierWin = 0x0008u;
inline constexpr std::uint32_t kModifierMask =
    kModifierAlt | kModifierControl | kModifierShift | kModifierWin;
inline constexpr std::uint32_t kModifierNoRepeat = 0x4000u;
inline constexpr std::size_t kMaxHotkeysPerPolicy = 128;

}  // namespace HotkeyPolicyConstants

enum class HotkeyMode : std::uint32_t {
    BlockAll = 0,
    Blacklist = 1,
    Whitelist = 2,
};

struct HotkeySpec {
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;

    friend bool operator==(const HotkeySpec& left, const HotkeySpec& right) noexcept {
        return left.modifiers == right.modifiers && left.virtualKey == right.virtualKey;
    }
};

struct HotkeyPolicy {
    HotkeyMode mode = HotkeyMode::BlockAll;
    std::vector<HotkeySpec> hotkeys;
};

inline HotkeySpec NormalizeHotkey(HotkeySpec hotkey) noexcept {
    hotkey.modifiers &= HotkeyPolicyConstants::kModifierMask;
    return hotkey;
}

inline bool IsValidHotkey(const HotkeySpec& hotkey) noexcept {
    return hotkey.virtualKey > 0 && hotkey.virtualKey <= 0xffu;
}

inline void NormalizeHotkeyPolicy(HotkeyPolicy& policy) {
    std::vector<HotkeySpec> normalized;
    normalized.reserve(policy.hotkeys.size());
    for (HotkeySpec hotkey : policy.hotkeys) {
        hotkey = NormalizeHotkey(hotkey);
        if (IsValidHotkey(hotkey)) {
            normalized.push_back(hotkey);
        }
    }

    std::sort(normalized.begin(), normalized.end(), [](const HotkeySpec& left,
                                                       const HotkeySpec& right) {
        if (left.modifiers != right.modifiers) {
            return left.modifiers < right.modifiers;
        }
        return left.virtualKey < right.virtualKey;
    });
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    policy.hotkeys = std::move(normalized);
}

inline bool IsValidHotkeyMode(HotkeyMode mode) noexcept {
    return mode == HotkeyMode::BlockAll || mode == HotkeyMode::Blacklist ||
           mode == HotkeyMode::Whitelist;
}

inline bool ValidateHotkeyPolicy(const HotkeyPolicy& policy) noexcept {
    if (!IsValidHotkeyMode(policy.mode) ||
        policy.hotkeys.size() > HotkeyPolicyConstants::kMaxHotkeysPerPolicy) {
        return false;
    }
    for (const HotkeySpec& hotkey : policy.hotkeys) {
        if (NormalizeHotkey(hotkey) != hotkey || !IsValidHotkey(hotkey)) {
            return false;
        }
    }
    return true;
}

inline bool ContainsHotkey(const HotkeyPolicy& policy, HotkeySpec hotkey) noexcept {
    hotkey = NormalizeHotkey(hotkey);
    return std::find(policy.hotkeys.begin(), policy.hotkeys.end(), hotkey) !=
           policy.hotkeys.end();
}

inline bool ShouldBlockHotkey(const HotkeyPolicy& policy, HotkeySpec hotkey) noexcept {
    if (policy.mode == HotkeyMode::BlockAll) {
        return true;
    }
    const bool matched = ContainsHotkey(policy, hotkey);
    return policy.mode == HotkeyMode::Blacklist ? matched : !matched;
}

inline bool SameHotkeyPolicy(const HotkeyPolicy& left, const HotkeyPolicy& right) {
    HotkeyPolicy normalizedLeft = left;
    HotkeyPolicy normalizedRight = right;
    NormalizeHotkeyPolicy(normalizedLeft);
    NormalizeHotkeyPolicy(normalizedRight);
    return normalizedLeft.mode == normalizedRight.mode &&
           normalizedLeft.hotkeys == normalizedRight.hotkeys;
}
