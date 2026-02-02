#pragma once

#include "syntax/Ast.hpp"

#include <cstdint>
#include <vector>

namespace quartz::lsp {

enum class NodeKind : std::uint8_t {
    Decl,
    DeclName,
    Stmt,
    Expr,
    Type,
    Pattern,
    Param,   // owner: FnDecl or LambdaExpr
    Field,   // owner: StructDecl
    Variant, // owner: EnumDecl
    LetName,
    ForName,
    MatchArm,
};

struct Node {
    NodeKind kind;
    Span span;
    const void* ptr;
    const void* owner = nullptr;
};

/// Nodes containing `offset`, outermost first. `inclusiveEnd` also accepts a node ending exactly at `offset`.
std::vector<Node> locate(const syntax::Module& m, std::uint32_t offset, bool inclusiveEnd);

} // namespace quartz::lsp
