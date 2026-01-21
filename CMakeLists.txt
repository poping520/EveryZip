cmake_minimum_required(VERSION 3.20)

project(EveryArchive LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EveryArchive WIN32
        src/resource.h
        src/main.cpp
        src/types.h
        src/file_scanner.h
        src/file_scanner.cpp
        src/database.h
        src/database.cpp
        src/logger.cpp
        src/parser/archive_parser.h
        src/parser/zip_archive_parser.h
        src/parser/zip_archive_parser.cpp

        external/sqlite/sqlite3.c
)

add_subdirectory(test)
add_subdirectory(external/zlib)

add_library(minizip STATIC
        external/zlib/contrib/minizip/ioapi.c
        external/zlib/contrib/minizip/iowin32.c
        external/zlib/contrib/minizip/mztools.c
        external/zlib/contrib/minizip/unzip.c
)

target_include_directories(minizip PUBLIC
        external/zlib
        external/zlib/contrib/minizip
)

target_link_libraries(minizip PUBLIC zlibstatic)

target_include_directories(EveryArchive PRIVATE
        external/sqlite
        external/zlib
        external/zlib/contrib/minizip
)

target_compile_definitions(EveryArchive PRIVATE UNICODE _UNICODE)

target_link_libraries(EveryArchive PRIVATE
        comctl32
        shlwapi
        minizip
)
