set(_hkb_nsis_template_candidates
        "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in"
        "${CMAKE_ROOT}/Modules/NSIS.template.in")

set(_hkb_nsis_template_source "")
foreach (_candidate IN LISTS _hkb_nsis_template_candidates)
    if (EXISTS "${_candidate}")
        set(_hkb_nsis_template_source "${_candidate}")
        break()
    endif ()
endforeach ()

if (_hkb_nsis_template_source STREQUAL "")
    message(FATAL_ERROR "Could not find CMake's NSIS.template.in to prepare the Hotkey Blocker installer.")
endif ()

file(READ "${_hkb_nsis_template_source}" _hkb_nsis_template)
string(REPLACE "\r\n" "\n" _hkb_nsis_template "${_hkb_nsis_template}")

string(FIND "${_hkb_nsis_template}" [=[  !include "MUI.nsh"]=] _hkb_mui_include_position)
string(FIND "${_hkb_nsis_template}" [=[  Var IS_DEFAULT_INSTALLDIR]=] _hkb_variables_position)
string(FIND "${_hkb_nsis_template}" [=[  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES]=] _hkb_uninstaller_pages_position)
string(FIND "${_hkb_nsis_template}" [=[Function un.onInit
]=] _hkb_uninstaller_init_position)

if (_hkb_mui_include_position LESS 0 OR
        _hkb_variables_position LESS 0 OR
        _hkb_uninstaller_pages_position LESS 0 OR
        _hkb_uninstaller_init_position LESS 0)
    message(FATAL_ERROR "CMake's NSIS template no longer has the anchors required for the Hotkey Blocker uninstaller page.")
endif ()

string(REPLACE [=[  !include "MUI.nsh"]=] [=[  !include "MUI.nsh"
  !include "nsDialogs.nsh"]=] _hkb_nsis_template "${_hkb_nsis_template}")

string(REPLACE [=[  Var IS_DEFAULT_INSTALLDIR]=] [=[  Var IS_DEFAULT_INSTALLDIR
  Var HKB_DELETE_USER_DATA
  Var HKB_UNINSTALL_DIALOG
  Var HKB_UNINSTALL_CHECKBOX]=] _hkb_nsis_template "${_hkb_nsis_template}")

string(REPLACE [=[  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES]=] [=[  !insertmacro MUI_UNPAGE_CONFIRM
  UninstPage custom un.HkbUserDataPageCreate un.HkbUserDataPageLeave
  !insertmacro MUI_UNPAGE_INSTFILES]=] _hkb_nsis_template "${_hkb_nsis_template}")

string(REPLACE [=[Function un.onInit
]=] [=[Function un.HkbUserDataPageCreate
  !insertmacro MUI_HEADER_TEXT "卸载用户数据" "选择是否同时删除配置和日志"
  nsDialogs::Create 1018
  Pop $HKB_UNINSTALL_DIALOG
  StrCmp $HKB_UNINSTALL_DIALOG error 0 +2
    Abort
  ${NSD_CreateLabel} 0 0 100% 24u "卸载会移除 Hotkey Blocker 程序。配置文件和日志默认保留。"
  Pop $0
  ${NSD_CreateCheckbox} 0 32u 100% 14u "删除配置文件和日志"
  Pop $HKB_UNINSTALL_CHECKBOX
  ${NSD_SetState} $HKB_UNINSTALL_CHECKBOX 0
  nsDialogs::Show
FunctionEnd

Function un.HkbUserDataPageLeave
  ${NSD_GetState} $HKB_UNINSTALL_CHECKBOX $HKB_DELETE_USER_DATA
FunctionEnd

Function un.onInit
]=] _hkb_nsis_template "${_hkb_nsis_template}")

set(_hkb_nsis_module_dir "${CMAKE_CURRENT_BINARY_DIR}/cpack-nsis")
file(MAKE_DIRECTORY "${_hkb_nsis_module_dir}")
file(WRITE "${_hkb_nsis_module_dir}/NSIS.template.in" "${_hkb_nsis_template}")
list(PREPEND CMAKE_MODULE_PATH "${_hkb_nsis_module_dir}")
