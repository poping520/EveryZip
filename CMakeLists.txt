cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(EveryZip LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(EveryZip WIN32
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
        src/parser/archive_parser_factory.h
        src/parser/archive_parser_factory.cpp
        src/parser/zip_archive_parser.h
        src/parser/zip_archive_parser.cpp
        src/parser/sevenzip_archive_parser.h
        src/parser/sevenzip_archive_parser.cpp
        # src/parser/libarchive_parser.h
        # src/parser/libarchive_parser.cpp

        external/sqlite/sqlite3.c
)

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

add_library(sevenzip_c STATIC
        external/7zip/C/7zAlloc.c
        external/7zip/C/7zBuf.c
        external/7zip/C/7zFile.c
        external/7zip/C/7zDec.c
        external/7zip/C/7zArcIn.c
        external/7zip/C/7zStream.c
        external/7zip/C/Bcj2.c
        external/7zip/C/Bra.c
        external/7zip/C/Bra86.c
        external/7zip/C/BraIA64.c
        external/7zip/C/CpuArch.c
        external/7zip/C/Delta.c
        external/7zip/C/Lzma2Dec.c
        external/7zip/C/LzmaDec.c
        external/7zip/C/Ppmd7.c
        external/7zip/C/Ppmd7Dec.c
        external/7zip/C/7zCrc.c
        external/7zip/C/7zCrcOpt.c
)

target_include_directories(sevenzip_c PUBLIC
        external/7zip/C
)

target_compile_definitions(sevenzip_c PUBLIC
        Z7_EXTRACT_ONLY
        Z7_PPMD_SUPPORT
        USE_C_CRC
)

target_include_directories(EveryZip PRIVATE
        external/sqlite
        external/zlib
        external/zlib/contrib/minizip
        external/7zip/C
)

target_compile_definitions(EveryZip PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(EveryZip PRIVATE /utf-8)
endif()

if(MSVC)
    # manifest 已通过 RC 文件嵌入（strings.rc: 1 RT_MANIFEST "app.manifest"），
    # 禁止链接器自动生成并合并 manifest，避免覆盖 DPI 感知声明
    target_link_options(EveryZip PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(EveryZip PRIVATE
        comctl32
        shlwapi
        minizip
        sevenzip_c
)

add_subdirectory(test)
