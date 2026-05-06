#pragma once

#include <string_view>
#include <utility>
#include <vector>

namespace quartz::stdlib {

/// Appends the (file name, text) of the packages in this folder to the standard library sources.
void addLibrarySources(std::vector<std::pair<std::string_view, std::string_view>>& out);

// Quartz sources of the standard library packages added after the first ones (Sources.cpp), one file each.
extern const std::string_view kCryptoQzi;
extern const std::string_view kEncodingQz;
extern const std::string_view kEncodingQzi;
extern const std::string_view kTimeQz;
extern const std::string_view kDecimalQz;
extern const std::string_view kDecimalQzi;
extern const std::string_view kTextQz;
extern const std::string_view kTextQzi;
extern const std::string_view kFormatsQzi;
extern const std::string_view kBytesQz;
extern const std::string_view kBitsQzi;

} // namespace quartz::stdlib
