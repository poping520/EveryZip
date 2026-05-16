#pragma once

#include <string>

#define EVERYZIP_VERSION_MAJOR 0
#define EVERYZIP_VERSION_MINOR 9
#define EVERYZIP_VERSION_PATCH 8

namespace EveryZip {

inline std::string AppVersionString() {
    return std::to_string(EVERYZIP_VERSION_MAJOR) + "." +
        std::to_string(EVERYZIP_VERSION_MINOR) + "." +
        std::to_string(EVERYZIP_VERSION_PATCH);
}

inline std::wstring AppVersionWString() {
    return std::to_wstring(EVERYZIP_VERSION_MAJOR) + L"." +
        std::to_wstring(EVERYZIP_VERSION_MINOR) + L"." +
        std::to_wstring(EVERYZIP_VERSION_PATCH);
}

} // namespace EveryZip
