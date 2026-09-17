cmake_minimum_required(VERSION 4.2)
project(hkb LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HKB_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin" CACHE PATH
        "Directory shared by the x64 and Win32 runtime artifacts")

set(HKB_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/hkb.manifest")
set_source_files_properties("${HKB_MANIFEST}" PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(detours STATIC
        third_party/detours/src/detours.cpp
        third_party/detours/src/modules.cpp
        third_party/detours/src/disasm.cpp
)

target_include_directories(detours PUBLIC
        third_party/detours/src
)

target_compile_definitions(detours PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
)

if (MSVC)
    target_compile_options(detours PRIVATE /utf-8)
endif ()

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
        "${HKB_MANIFEST}"
)

target_include_directories(hkb PRIVATE
        "third_party/wtl"
        "src"
)

target_compile_definitions(hkb PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(hkb PRIVATE /utf-8)
endif ()

target_link_libraries(hkb PRIVATE
        advapi32
        comctl32
        ole32
        shell32
)

if (MSVC)
    add_custom_command(TARGET hkb POST_BUILD
            COMMAND "${CMAKE_MT}"
                    -manifest "${HKB_MANIFEST}"
                    "-outputresource:$<TARGET_FILE:hkb>;#1"
            VERBATIM
    )
endif ()

set(HKB_ARCH_SUFFIX 32)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(HKB_ARCH_SUFFIX 64)
endif ()

add_library(hotkey_hook SHARED
        hook/HookMain.cpp
        hook/RegisterHotKeyHook.cpp
)

target_include_directories(hotkey_hook PRIVATE
        hook
)

target_link_libraries(hotkey_hook PRIVATE
        detours
        user32
)

target_compile_definitions(hotkey_hook PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
)

if (MSVC)
    target_compile_options(hotkey_hook PRIVATE /utf-8)
endif ()

set_target_properties(hkb hotkey_hook PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}"
)
set_target_properties(hkb PROPERTIES
        OUTPUT_NAME "HotkeyBlocker"
)
set_target_properties(hotkey_hook PROPERTIES
        OUTPUT_NAME "HotkeyHook${HKB_ARCH_SUFFIX}"
)

if (CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(hotkey_blocker_injector32
            injector32/main.cpp
            src/RemoteInjector.cpp
    )

    target_include_directories(hotkey_blocker_injector32 PRIVATE
            src
    )

    target_compile_definitions(hotkey_blocker_injector32 PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0601
    )

    if (MSVC)
        target_compile_options(hotkey_blocker_injector32 PRIVATE /utf-8)
    endif ()

    target_link_libraries(hotkey_blocker_injector32 PRIVATE
            user32
    )
    set_target_properties(hotkey_blocker_injector32 PROPERTIES
            OUTPUT_NAME "HotkeyBlockerInjector32"
            RUNTIME_OUTPUT_DIRECTORY "${HKB_OUTPUT_DIR}"
    )
endif ()

option(HKB_BUILD_TESTS "Build Hotkey Blocker tests" ON)

if (HKB_BUILD_TESTS)
    enable_testing()

    add_executable(hkb_config_store_test
            tests/ConfigStoreTest.cpp
            src/ConfigStore.cpp
            src/PathUtils.cpp
            src/RuleManager.cpp
    )

    target_include_directories(hkb_config_store_test PRIVATE
            src
    )

    target_compile_definitions(hkb_config_store_test PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
    )

    if (MSVC)
        target_compile_options(hkb_config_store_test PRIVATE /utf-8)
    endif ()

    target_link_libraries(hkb_config_store_test PRIVATE
            ole32
            shell32
    )

    add_test(NAME hkb_config_store_round_trip COMMAND hkb_config_store_test)

    add_executable(hotkey_probe tests/HotkeyProbe.cpp)
    target_compile_definitions(hotkey_probe PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
    )
    if (MSVC)
        target_compile_options(hotkey_probe PRIVATE /utf-8)
    endif ()
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
        )
        target_include_directories(hkb_injection_test PRIVATE src)
        target_compile_definitions(hkb_injection_test PRIVATE
                UNICODE
                _UNICODE
                WIN32_LEAN_AND_MEAN
        )
        if (MSVC)
            target_compile_options(hkb_injection_test PRIVATE /utf-8)
        endif ()
        target_link_libraries(hkb_injection_test PRIVATE user32)
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
                src/Logger.cpp
                src/PathUtils.cpp
                src/ProcessMonitor.cpp
                src/RemoteInjector.cpp
        )
        target_include_directories(hkb_blocker_service_integration_test PRIVATE src)
        target_compile_definitions(hkb_blocker_service_integration_test PRIVATE
                UNICODE
                _UNICODE
                WIN32_LEAN_AND_MEAN
        )
        if (MSVC)
            target_compile_options(hkb_blocker_service_integration_test PRIVATE /utf-8)
        endif ()
        target_link_libraries(hkb_blocker_service_integration_test PRIVATE
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
