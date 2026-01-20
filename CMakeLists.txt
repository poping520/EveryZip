cmake_minimum_required(VERSION 3.20)

project(EveryArchive LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EveryArchive WIN32
        src/resource.h
        src/main.cpp
        src/logger.cpp
        external/sqlite/sqlite3.c
)

target_include_directories(EveryArchive PRIVATE
        external/sqlite
)

target_compile_definitions(EveryArchive PRIVATE UNICODE _UNICODE)

target_link_libraries(EveryArchive PRIVATE
        comctl32
        shlwapi
)
