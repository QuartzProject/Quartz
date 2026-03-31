#pragma once

#include "ir/Ir.hpp"
#include "sema/Sema.hpp"
#include "support/Diagnostics.hpp"
#include "syntax/Ast.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace quartz::lsp {

class Index;

struct Document {
    std::string uri;
    std::string path; // empty for non-file URIs
    std::string text;
    std::int64_t version = 0;
};

/// One program: the front end run over a folder's sources, kept alive for queries.
class Analysis {
public:
    struct Input {
        std::string uri; // empty for embedded std and host sources
        std::string name;
        std::string text;
    };

    explicit Analysis(std::vector<Input> inputs);
    Analysis(const Analysis&) = delete;
    Analysis& operator=(const Analysis&) = delete;
    ~Analysis();

    [[nodiscard]] std::optional<std::uint32_t> fileOf(std::string_view uri) const;

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diags_.all(); }

    [[nodiscard]] const Input& input(std::uint32_t file) const { return inputs_[file]; }

    [[nodiscard]] std::size_t fileCount() const { return inputs_.size(); }

    [[nodiscard]] syntax::Module* module(std::uint32_t file) const {
        return file < modules_.size() ? modules_[file] : nullptr;
    }

    [[nodiscard]] const std::vector<syntax::Module*>& modules() const { return modules_; }

    [[nodiscard]] const sema::Program* program() const { return program_.get(); }

    [[nodiscard]] const Interner& names() const { return names_; }

    [[nodiscard]] std::string typeName(std::uint32_t t) const;

    [[nodiscard]] double millis() const { return millis_; }

    /// Resolved names of the user files, built on first use.
    [[nodiscard]] const Index& index() const;

private:
    std::vector<Input> inputs_;
    SourceManager sources_;
    Diagnostics diags_{sources_};
    Arena arena_;
    Interner names_;
    std::vector<syntax::Module*> modules_;
    std::unique_ptr<sema::Program> program_;
    double millis_ = 0;
    mutable std::unique_ptr<Index> index_;
};

/// Open documents grouped by folder; each folder is analyzed as one program.
class Workspace {
public:
    void open(const std::string& uri, std::string text, std::int64_t version);
    void change(const std::string& uri, std::string text, std::int64_t version);
    void close(const std::string& uri);

    [[nodiscard]] const Document* document(const std::string& uri) const;
    /// The up-to-date analysis covering `uri`, recomputing it when stale.
    Analysis* analysisFor(const std::string& uri);
    /// Folders whose documents changed since their last analysis.
    [[nodiscard]] std::vector<std::string> dirtyFolders() const;
    [[nodiscard]] bool isDirty(const std::string& folder) const;
    Analysis* analyzeFolder(const std::string& folder);
    [[nodiscard]] std::vector<std::string> documentsIn(const std::string& folder) const;
    [[nodiscard]] std::string folderOf(const std::string& uri) const;

private:
    struct Folder {
        std::unique_ptr<Analysis> analysis;
        bool dirty = true;
    };

    std::vector<Analysis::Input> inputsFor(const std::string& folder) const;

    std::map<std::string, Document> docs_;
    std::map<std::string, Folder> folders_;
};

} // namespace quartz::lsp
