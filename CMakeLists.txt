cmake_minimum_required(VERSION 3.25)

set(HKB_PROJECT_VERSION "1.0.0" CACHE STRING "Hotkey Blocker project version")
project(HotkeyBlocker VERSION ${HKB_PROJECT_VERSION} LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(DETOURS_INCLUDE_DIR NAMES detours/detours.h REQUIRED)
find_library(DETOURS_LIBRARY NAMES detours REQUIRED)
find_path(WTL_INCLUDE_DIR NAMES atlapp.h REQUIRED)

add_library(Detours::Detours UNKNOWN IMPORTED GLOBAL)
set_target_properties(Detours::Detours PROPERTIES
        IMPORTED_LOCATION "${DETOURS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DETOURS_INCLUDE_DIR}"
)

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
        if (VCPKG_TARGET_TRIPLET MATCHES "-static$")
            # Static triplets keep the shipped binaries independent of the VC++ runtime.
            set(_hkb_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        else ()
            set(_hkb_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif ()
        set_property(TARGET "${target}" PROPERTY MSVC_RUNTIME_LIBRARY
                "${_hkb_msvc_runtime}")
    endif ()
endfunction()

configure_file(
        "${CMAKE_SOURCE_DIR}/src/version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        @ONLY
)

configure_file(
        "${CMAKE_SOURCE_DIR}/src/Version.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
        @ONLY
)

add_executable(hkb WIN32
        src/ArchitectureDetector.cpp
        src/BlockerService.cpp
        src/ConfigStore.cpp
        src/Injector.cpp
        src/InjectionOperationReaper.cpp
        src/HotkeyPolicyTransport.cpp
        src/Logger.cpp
        src/PathUtils.cpp
        src/ProcessMonitor.cpp
        src/RemoteInjector.cpp
        src/RuleManager.cpp
        src/StartupManager.cpp
        src/UpdateChecker.cpp
        src/Win32Support.cpp
        src/main.cpp
        src/app.rc
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)
target_include_directories(hkb PRIVATE "${WTL_INCLUDE_DIR}" src "${CMAKE_CURRENT_BINARY_DIR}")
hkb_configure_target(hkb)
if (MSVC)
    target_link_options(hkb PRIVATE
            "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
    )
endif ()
target_link_libraries(hkb PRIVATE
        advapi32
        comctl32
        cpr::cpr
        gdi32
        nlohmann_json::nlohmann_json
        ole32
        shell32
        user32
)

add_library(hotkey_hook SHARED
        hook/HookMain.cpp
        hook/RegisterHotKeyHook.cpp
)
target_include_directories(hotkey_hook PRIVATE hook src)
hkb_configure_target(hotkey_hook)
target_sources(hotkey_hook PRIVATE src/HotkeyPolicyTransport.cpp)
target_link_libraries(hotkey_hook PRIVATE Detours::Detours user32)

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
            src/Win32Support.cpp
    )
    target_include_directories(hotkey_blocker_injector32 PRIVATE src)
    hkb_configure_target(hotkey_blocker_injector32)
    target_link_libraries(hotkey_blocker_injector32 PRIVATE ole32 shell32 user32)
    set_target_properties(hotkey_blocker_injector32 PROPERTIES
            OUTPUT_NAME HotkeyBlockerInjector32)
endif ()

option(HKB_BUILD_TESTS "Build Hotkey Blocker tests" ON)

if (HKB_BUILD_TESTS)
    enable_testing()

    add_executable(hkb_config_store_test
            tests/ConfigStoreTest.cpp
            src/ConfigStore.cpp
            src/PathUtils.cpp
            src/RuleManager.cpp
            src/Win32Support.cpp
    )
    target_include_directories(hkb_config_store_test PRIVATE src)
    hkb_configure_target(hkb_config_store_test)
    target_link_libraries(hkb_config_store_test PRIVATE ole32 shell32)
    add_test(NAME hkb_config_store_round_trip COMMAND hkb_config_store_test)

    add_executable(hotkey_probe tests/HotkeyProbe.cpp)
    hkb_configure_target(hotkey_probe)
    target_link_libraries(hotkey_probe PRIVATE user32)
    set_target_properties(hotkey_probe PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}"
    )

    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_executable(hkb_injection_test
                tests/HotkeyInjectionTest.cpp
                src/ArchitectureDetector.cpp
                src/Injector.cpp
                src/RemoteInjector.cpp
                src/Win32Support.cpp
        )
        target_include_directories(hkb_injection_test PRIVATE src)
        hkb_configure_target(hkb_injection_test)
        target_link_libraries(hkb_injection_test PRIVATE ole32 shell32 user32)
        add_dependencies(hkb_injection_test hotkey_hook hotkey_probe)
        set_target_properties(hkb_injection_test PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}"
        )
        add_test(NAME hkb_register_hotkey_injection COMMAND hkb_injection_test)

        add_executable(hkb_blocker_service_integration_test
                tests/BlockerServiceIntegrationTest.cpp
                src/ArchitectureDetector.cpp
                src/BlockerService.cpp
                src/Injector.cpp
                src/InjectionOperationReaper.cpp
                src/HotkeyPolicyTransport.cpp
                src/Logger.cpp
                src/PathUtils.cpp
                src/ProcessMonitor.cpp
                src/RemoteInjector.cpp
                src/Win32Support.cpp
        )
        target_include_directories(hkb_blocker_service_integration_test PRIVATE src)
        hkb_configure_target(hkb_blocker_service_integration_test)
        target_link_libraries(hkb_blocker_service_integration_test PRIVATE
                ole32
                shell32
                user32
        )
        add_dependencies(hkb_blocker_service_integration_test hotkey_hook hotkey_probe)
        set_target_properties(hkb_blocker_service_integration_test PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}"
        )
        add_test(NAME hkb_blocker_service_integration
                COMMAND hkb_blocker_service_integration_test)
    endif ()
endif ()

install(TARGETS hkb
        RUNTIME DESTINATION .
)

install(FILES
        README.md
        LICENSE
        THIRD_PARTY_NOTICES.md
        DESTINATION .
)
install(FILES
        docs/USER_GUIDE.md
        DESTINATION docs
)
set(HKB_VCPKG_SHARE_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
install(FILES "${HKB_VCPKG_SHARE_DIR}/detours/copyright"
        DESTINATION licenses/detours)
install(FILES "${HKB_VCPKG_SHARE_DIR}/wtl/copyright"
        DESTINATION licenses/wtl)
install(FILES "${HKB_VCPKG_SHARE_DIR}/cpr/copyright"
        DESTINATION licenses/cpr)
install(FILES "${HKB_VCPKG_SHARE_DIR}/curl/copyright"
        DESTINATION licenses/curl)
install(FILES "${HKB_VCPKG_SHARE_DIR}/nlohmann-json/copyright"
        DESTINATION licenses/nlohmann-json)
install(FILES "${HKB_VCPKG_SHARE_DIR}/zlib/copyright"
        DESTINATION licenses/zlib)

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    install(TARGETS hotkey_hook hotkey_blocker_injector32
            RUNTIME DESTINATION win32
    )
else ()
    install(TARGETS hotkey_hook
            RUNTIME DESTINATION .
    )

    set(HKB_WIN32_RUNTIME_DIR "${HKB_OUTPUT_DIR}/win32" CACHE PATH
            "Directory containing the 32-bit Hook DLL and injector for an x64 package")
    install(FILES
            "${HKB_WIN32_RUNTIME_DIR}/HotkeyHook32.dll"
            "${HKB_WIN32_RUNTIME_DIR}/HotkeyBlockerInjector32.exe"
            DESTINATION win32
    )
endif ()

set(CPACK_GENERATOR NSIS)
set(CPACK_NSIS_COMPONENT_INSTALL OFF)
set(CPACK_PACKAGE_NAME HotkeyBlocker)
set(CPACK_PACKAGE_VENDOR "Hotkey Blocker")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Windows utility for blocking selected applications from registering global hotkeys")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/halifox/hotkey_blocker")
set(CPACK_PACKAGE_CONTACT "Hotkey Blocker contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Hotkey Blocker")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${HKB_ARCH_LABEL}")
set(CPACK_PACKAGE_EXECUTABLES "HotkeyBlocker" "Hotkey Blocker")
set(CPACK_CREATE_DESKTOP_LINKS HotkeyBlocker)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/assets/HotkeyBlocker.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/assets/HotkeyBlocker.ico")
set(CPACK_NSIS_DISPLAY_NAME "Hotkey Blocker")
set(CPACK_NSIS_PACKAGE_NAME "Hotkey Blocker")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "HotkeyBlocker.exe")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "ExecWait '$SYSDIR\\\\taskkill.exe /F /T /IM HotkeyBlocker.exe'\nDeleteRegValue HKCU Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run HotkeyBlocker\nRMDir /r \\\"$LOCALAPPDATA\\\\HotkeyBlocker\\\"")
set(CPACK_NSIS_HELP_LINK "https://github.com/halifox/hotkey_blocker/blob/master/docs/USER_GUIDE.md")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/halifox/hotkey_blocker")

include(CPack)
