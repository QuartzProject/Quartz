#pragma once

#include "Imports.hpp"
#include "Workspace.hpp"

#include <optional>
#include <string>
#include <vector>

namespace quartz::lsp {

/// What the cursor points at: hover text, the span it covers and where it is declared.
struct Resolved {
    std::string hover; // Quartz code shown in a fenced block
    Span range;
    std::optional<Span> declaration;
};

std::optional<Resolved> resolveAt(const Analysis& a, std::uint32_t file, std::uint32_t offset);

struct Completion {
    std::string label;
    int kind = 0; // LSP CompletionItemKind
    std::string detail;
    std::string package;    // set on symbols that are not in scope yet
    std::string insertText; // when it differs from the label
    std::vector<TextEdit> edits;
};

std::vector<Completion> completeAt(const Analysis& a, std::uint32_t file, std::uint32_t offset);

/// `fn name(a: T) -> R` for a declared function or method.
std::string signature(const Analysis& a, const sema::FnInfo& fn);
/// The struct, state or enum declaration behind a nominal type (or its generic base).
const syntax::Decl* typeDecl(const Analysis& a, std::uint32_t type);
const syntax::EnumDecl* enumDeclOf(const Analysis& a, std::uint32_t type);
/// A struct, state or enum declared with this name, or 0.
std::uint32_t typeNamed(const Analysis& a, std::string_view name);

} // namespace quartz::lsp
