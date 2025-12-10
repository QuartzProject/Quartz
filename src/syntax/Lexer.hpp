#pragma once

#include "support/Arena.hpp"
#include "support/Diagnostics.hpp"
#include "syntax/Token.hpp"

#include <vector>

namespace quartz::syntax {

/// Tokenizes a whole file; decoded string contents live in `arena`. `comments`, when given, receives comment spans.
std::vector<Token> tokenize(const SourceFile& file, Arena& arena, Diagnostics& diags,
                            std::vector<Span>* comments = nullptr);

} // namespace quartz::syntax
