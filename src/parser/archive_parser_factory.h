#pragma once

#include <memory>
#include <string>

#include "archive_parser.h"

namespace EveryZip {

std::unique_ptr<IArchiveParser> CreateArchiveParserForPath(const std::wstring& archive_path);

} // namespace EveryZip
