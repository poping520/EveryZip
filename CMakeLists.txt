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
        src/parser/libarchive_parser.h
        src/parser/libarchive_parser.cpp

        external/sqlite/sqlite3.c
)

add_subdirectory(test)

# libarchive 静态编译，关闭所有外部可选依赖，仅使用内置解码器
set(ENABLE_TEST           OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR            OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO           OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT            OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP          OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL        OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL        OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2          OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4            OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO            OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA           OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD           OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2          OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2        OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT          OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX      OFF CACHE BOOL "" FORCE)
set(ENABLE_LibGCC         OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG            OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS        OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE         OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB           ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
add_subdirectory(external/libarchive)

target_include_directories(EveryArchive PRIVATE
        external/sqlite
        external/libarchive/libarchive
)

target_compile_definitions(EveryArchive PRIVATE UNICODE _UNICODE LIBARCHIVE_STATIC)

if(MSVC)
    target_compile_options(EveryArchive PRIVATE /utf-8)
endif()

if(MSVC)
    # manifest 已通过 RC 文件嵌入（strings.rc: 1 RT_MANIFEST "app.manifest"），
    # 禁止链接器自动生成并合并 manifest，避免覆盖 DPI 感知声明
    target_link_options(EveryArchive PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(EveryArchive PRIVATE
        comctl32
        shlwapi
        archive_static
)
