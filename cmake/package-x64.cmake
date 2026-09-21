if (NOT DEFINED HKB_PROJECT_VERSION OR HKB_PROJECT_VERSION STREQUAL "")
    set(HKB_PROJECT_VERSION "1.0.0")
endif ()

if (NOT HKB_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
            "HKB_PROJECT_VERSION must use MAJOR.MINOR.PATCH, got: ${HKB_PROJECT_VERSION}")
endif ()

if (DEFINED VCPKG_ROOT AND NOT VCPKG_ROOT STREQUAL "")
    set(ENV{VCPKG_ROOT} "${VCPKG_ROOT}")
elseif (DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
else ()
    message(FATAL_ERROR
            "Set VCPKG_ROOT or pass -DVCPKG_ROOT=<path> to this command.")
endif ()

if (NOT EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "Invalid VCPKG_ROOT: $ENV{VCPKG_ROOT}")
endif ()

if (NOT DEFINED ENV{VSINSTALLDIR} OR "$ENV{VSINSTALLDIR}" STREQUAL "")
    message(FATAL_ERROR
            "Run this command from a Visual Studio Developer PowerShell so VSINSTALLDIR is available.")
endif ()

file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _vs_install_dir)
cmake_path(NORMAL_PATH _vs_install_dir)
cmake_path(APPEND _vs_install_dir "Common7" "Tools" "VsDevCmd.bat"
        OUTPUT_VARIABLE _vs_dev_cmd)
if (NOT EXISTS "${_vs_dev_cmd}")
    message(FATAL_ERROR "Could not find VsDevCmd.bat at ${_vs_dev_cmd}")
endif ()

get_filename_component(_source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(TO_NATIVE_PATH "${VCPKG_ROOT}" _vcpkg_root_native)
file(TO_NATIVE_PATH "${_source_dir}" _source_dir_native)
file(TO_NATIVE_PATH "${_vs_dev_cmd}" _vs_dev_cmd_native)

set(_driver_dir "${_source_dir}/out/build")
file(MAKE_DIRECTORY "${_driver_dir}")

function(hkb_run_vs_build architecture driver_name)
    set(_driver_file "${_driver_dir}/${driver_name}")
    set(_driver_content "@echo off\r\nsetlocal\r\n")
    string(APPEND _driver_content "set \"VCPKG_ROOT=${_vcpkg_root_native}\"\r\n")
    string(APPEND _driver_content "cd /d \"${_source_dir_native}\"\r\n")
    string(APPEND _driver_content "if errorlevel 1 exit /b 1\r\n")
    string(APPEND _driver_content
            "call \"${_vs_dev_cmd_native}\" -arch=${architecture} -host_arch=x64\r\n")
    string(APPEND _driver_content "if errorlevel 1 exit /b 1\r\n")
    foreach (_command IN LISTS ARGN)
        string(APPEND _driver_content "${_command}\r\n")
        string(APPEND _driver_content "if errorlevel 1 exit /b 1\r\n")
    endforeach ()
    file(WRITE "${_driver_file}" "${_driver_content}")
    execute_process(
            COMMAND "$ENV{COMSPEC}" /D /S /C CALL "${_driver_file}"
            WORKING_DIRECTORY "${_source_dir}"
            RESULT_VARIABLE _build_result
    )
    file(REMOVE "${_driver_file}")
    if (NOT _build_result STREQUAL "0")
        message(FATAL_ERROR "${architecture} package build failed with exit code: ${_build_result}")
    endif ()
endfunction()

set(_version_argument "-DHKB_PROJECT_VERSION=${HKB_PROJECT_VERSION}")
hkb_run_vs_build(x86 "package-x64-x86.cmd"
        "\"${CMAKE_COMMAND}\" --fresh --preset x86-release \"${_version_argument}\""
        "\"${CMAKE_COMMAND}\" --build --preset x86-release --target hotkey_hook hotkey_blocker_injector32 --parallel")
hkb_run_vs_build(x64 "package-x64-x64.cmd"
        "\"${CMAKE_COMMAND}\" --fresh --preset x64-release \"${_version_argument}\""
        "\"${CMAKE_COMMAND}\" --build --preset x64-release --target hkb hotkey_hook --parallel"
        "\"${CMAKE_COMMAND}\" --build --preset x64-release --target package --parallel")

set(_package_name "HotkeyBlocker-${HKB_PROJECT_VERSION}-x64.exe")
set(_built_package "${_source_dir}/out/build/x64-release-vcpkg/${_package_name}")
set(_package_dir "${_source_dir}/out/packages")
set(_package_file "${_package_dir}/${_package_name}")
if (NOT EXISTS "${_built_package}")
    message(FATAL_ERROR "CPack did not create the expected installer: ${_built_package}")
endif ()

file(MAKE_DIRECTORY "${_package_dir}")
file(COPY_FILE "${_built_package}" "${_package_file}" ONLY_IF_DIFFERENT)
file(SHA256 "${_package_file}" _package_hash)
file(WRITE "${_package_file}.sha256" "${_package_hash}  ${_package_name}\n")
message(STATUS "Package: ${_package_file}")
message(STATUS "SHA-256: ${_package_file}.sha256")
