cmake_minimum_required(VERSION 3.20)

project(EveryArchive LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_executable(TestZip
        test_zip.cpp
        ../src/parser/archive_parser.h
        ../src/parser/zip_archive_parser.h
        ../src/parser/zip_archive_parser.cpp
)

add_executable(TestUsnScan
        test_usn_scan.cpp
)

add_executable(TestMftScan
        test_mft_scan.cpp
)

add_executable(TestFileTracker
        test_file_tracker.cpp
        ../external/sqlite/sqlite3.c
)

add_executable(QueryDB
        query_db.cpp
        ../external/sqlite/sqlite3.c
)

target_link_libraries(TestZip PRIVATE
        minizip
)

target_include_directories(TestZip PRIVATE
        ../src
        ../external/zlib
        ../external/zlib/contrib/minizip
)

target_include_directories(TestFileTracker PRIVATE
        ../external/sqlite
)

target_include_directories(QueryDB PRIVATE
        ../external/sqlite
)

add_executable(TestConfig
        test_config.cpp
        ../src/config/advconfig.cpp
        ../src/string_utils.cpp
        ../src/logger.cpp
)

target_include_directories(TestConfig PRIVATE
        ../src/config
)

target_link_libraries(TestConfig PRIVATE
        shlwapi
)

target_compile_definitions(TestConfig PRIVATE UNICODE _UNICODE)

target_compile_definitions(TestFileTracker PRIVATE UNICODE _UNICODE)