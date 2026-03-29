cmake_minimum_required(VERSION 3.20)

project(EveryArchive LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EveryArchive WIN32
        src/resource.h
        src/strings.rc
        src/main.cpp
        src/main_window.h
        src/main_window.cpp
        src/string_utils.h
        src/string_utils.cpp
        src/tray_icon.h
        src/tray_icon.cpp
        src/icon_cache.h
        src/icon_cache.cpp
        src/row_cache.h
        src/row_cache.cpp
        src/indexer.h
        src/indexer.cpp
        src/types.h
        src/file_scanner.h
        src/file_scanner.cpp
 
        src/database.h
        src/database.cpp
        src/logger.cpp
        
        src/config/advconfig.h
        src/config/advconfig.cpp
        src/config/user_config.h
        src/config/user_config.cpp

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

if(MSVC)
    target_compile_options(EveryArchive PRIVATE /utf-8)
endif()

# 嵌入自定义 manifest（请求管理员权限 + ComCtl32 v6 视觉样式）
set_target_properties(EveryArchive PROPERTIES
    LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

target_link_libraries(EveryArchive PRIVATE
        comctl32
        shlwapi
        minizip
)
