#include "std/lib/Library.hpp"

namespace quartz::stdlib {

void addLibrarySources(std::vector<std::pair<std::string_view, std::string_view>>& out) {
    out.insert(out.end(), {
                              {"<std>/crypto.qzi", kCryptoQzi},
                              {"<std>/encoding.qz", kEncodingQz},
                              {"<std>/encoding.qzi", kEncodingQzi},
                          });
}

} // namespace quartz::stdlib
