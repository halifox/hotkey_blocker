cmake_minimum_required(VERSION 4.2)
project(hkb LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hkb WIN32
        src/main.cpp
        src/app.rc
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
