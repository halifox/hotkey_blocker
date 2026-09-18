cmake_minimum_required(VERSION 3.25)

set(HKB_PROJECT_VERSION "1.0.0" CACHE STRING "Hotkey Blocker project version")
project(HotkeyBlocker VERSION ${HKB_PROJECT_VERSION} LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(HKB_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin" CACHE PATH
        "Directory for the runtime artifacts")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}")

set(HKB_WINDOWS_DEFINITIONS
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
)

function(hkb_configure_target target)
    target_compile_definitions("${target}" PRIVATE ${HKB_WINDOWS_DEFINITIONS})
    if (MSVC)
        target_compile_options("${target}" PRIVATE /utf-8)
    endif ()
endfunction()

configure_file(
        "${CMAKE_SOURCE_DIR}/src/version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        @ONLY
)

add_library(detours STATIC
        third_party/detours/src/detours.cpp
        third_party/detours/src/modules.cpp
        third_party/detours/src/disasm.cpp
)
target_include_directories(detours PUBLIC third_party/detours/src)
hkb_configure_target(detours)

add_executable(hkb WIN32
        src/ArchitectureDetector.cpp
        src/BlockerService.cpp
        src/ConfigStore.cpp
        src/Injector.cpp
        src/Logger.cpp
        src/PathUtils.cpp
        src/ProcessMonitor.cpp
        src/RemoteInjector.cpp
        src/RuleManager.cpp
        src/StartupManager.cpp
        src/main.cpp
        src/app.rc
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)
target_include_directories(hkb PRIVATE third_party/wtl src)
hkb_configure_target(hkb)
if (MSVC)
    target_link_options(hkb PRIVATE
            "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
    )
endif ()
target_link_libraries(hkb PRIVATE
        advapi32
        comctl32
        ole32
        shell32
        wbemuuid
)

add_library(hotkey_hook SHARED
        hook/HookMain.cpp
        hook/RegisterHotKeyHook.cpp
)
target_include_directories(hotkey_hook PRIVATE hook)
hkb_configure_target(hotkey_hook)
target_link_libraries(hotkey_hook PRIVATE detours user32)

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(HKB_ARCH_LABEL x64)
    set(HKB_HOOK_NAME HotkeyHook64)
else ()
    set(HKB_ARCH_LABEL x86)
    set(HKB_HOOK_NAME HotkeyHook32)
endif ()

set_target_properties(hkb PROPERTIES OUTPUT_NAME HotkeyBlocker)
set_target_properties(hotkey_hook PROPERTIES OUTPUT_NAME "${HKB_HOOK_NAME}")

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(hotkey_blocker_injector32
            injector32/main.cpp
            src/RemoteInjector.cpp
    )
    target_include_directories(hotkey_blocker_injector32 PRIVATE src)
    hkb_configure_target(hotkey_blocker_injector32)
    target_link_libraries(hotkey_blocker_injector32 PRIVATE user32)
    set_target_properties(hotkey_blocker_injector32 PROPERTIES
            OUTPUT_NAME HotkeyBlockerInjector32)
endif ()

install(TARGETS hkb
        RUNTIME DESTINATION .
        COMPONENT runtime
)

install(FILES
        README.md
        LICENSE
        THIRD_PARTY_NOTICES.md
        DESTINATION .
        COMPONENT documentation
)
install(FILES
        docs/USER_GUIDE.md
        DESTINATION docs
        COMPONENT documentation
)
install(FILES
        third_party/detours/LICENSE.md
        DESTINATION licenses/detours
        COMPONENT documentation
)

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    install(TARGETS hotkey_hook hotkey_blocker_injector32
            RUNTIME DESTINATION win32
            COMPONENT runtime
    )
else ()
    install(TARGETS hotkey_hook
            RUNTIME DESTINATION .
            COMPONENT runtime
    )

    set(HKB_WIN32_RUNTIME_DIR "${HKB_OUTPUT_DIR}/win32" CACHE PATH
            "Directory containing the 32-bit Hook DLL and injector for an x64 package")
    install(FILES
            "${HKB_WIN32_RUNTIME_DIR}/HotkeyHook32.dll"
            "${HKB_WIN32_RUNTIME_DIR}/HotkeyBlockerInjector32.exe"
            DESTINATION win32
            COMPONENT runtime
            OPTIONAL
    )
endif ()

set(CPACK_GENERATOR NSIS)
set(CPACK_NSIS_COMPONENT_INSTALL OFF)
set(CPACK_PACKAGE_NAME HotkeyBlocker)
set(CPACK_PACKAGE_VENDOR "Hotkey Blocker")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Block applications from registering global hotkeys")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/halifox/hkb")
set(CPACK_PACKAGE_CONTACT "Hotkey Blocker contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Hotkey Blocker")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${HKB_ARCH_LABEL}")
set(CPACK_PACKAGE_EXECUTABLES "HotkeyBlocker" "Hotkey Blocker")
set(CPACK_CREATE_DESKTOP_LINKS HotkeyBlocker)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_NSIS_DISPLAY_NAME "Hotkey Blocker")
set(CPACK_NSIS_PACKAGE_NAME "Hotkey Blocker")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_HELP_LINK "https://github.com/halifox/hkb/blob/master/docs/USER_GUIDE.md")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/halifox/hkb")

include(CPack)
