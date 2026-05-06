#include "std/lib/Library.hpp"

namespace quartz::stdlib {

void addLibrarySources(std::vector<std::pair<std::string_view, std::string_view>>& out) {
    out.insert(out.end(), {
                              {"<std>/crypto.qzi", kCryptoQzi},
                              {"<std>/encoding.qz", kEncodingQz},
                              {"<std>/encoding.qzi", kEncodingQzi},
                              {"<std>/time.qz", kTimeQz},
                              {"<std>/decimal.qz", kDecimalQz},
                              {"<std>/decimal.qzi", kDecimalQzi},
                              {"<std>/text.qz", kTextQz},
                              {"<std>/text.qzi", kTextQzi},
                              {"<std>/formats.qzi", kFormatsQzi},
                              {"<std>/bytes.qz", kBytesQz},
                              {"<std>/bits.qzi", kBitsQzi},
                              {"<std>/geometry.qz", kGeometryQz},
                              {"<std>/noise.qz", kNoiseQz},
                          });
}

} // namespace quartz::stdlib
