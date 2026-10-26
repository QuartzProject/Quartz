#include "Workspace.hpp"

#include "HostInterface.hpp"
#include "Index.hpp"
#include "Text.hpp"
#include "ir/Lower.hpp"
#include "std/Std.hpp"
#include "syntax/Parser.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace quartz::lsp {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUriFolder = "uri:";
constexpr std::string_view kQilnFolder = "qiln:";

bool isSource(std::string_view path) { return path.ends_with(".qz") || path.ends_with(".qzi"); }

// The `package` a file declares, read from its first line of code.
std::string packageOf(std::string_view text) {
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.starts_with("//")) {
            continue;
        }
        if (!line.starts_with("package ")) {
            return {};
        }
        line.remove_prefix(8);
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        return std::string(line.substr(0, line.find_first_of(" ;/")));
    }
    return {};
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Qiln writes the API that `build.qz` and `settings.qz` compile against to `.qiln/api` at the workspace root.
std::optional<fs::path> qilnApiFor(const fs::path& script) {
    std::string name = script.filename().string();
    if (name != "build.qz" && name != "settings.qz") {
        return std::nullopt;
    }
    std::error_code ec;
    for (fs::path dir = script.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        fs::path api = dir / ".qiln" / "api";
        if (fs::is_regular_file(api / "qiln.qzi", ec)) {
            return api;
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    return std::nullopt;
}

std::vector<fs::path> qilnApiSources(const fs::path& api) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(api, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (isSource(it->path().string()) && it->is_regular_file(ec)) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

Analysis::Analysis(std::vector<Input> inputs) : inputs_(std::move(inputs)) {
    auto start = std::chrono::steady_clock::now();
    for (const Input& in : inputs_) {
        const SourceFile& file = sources_.add(in.name, in.text);
        modules_.push_back(syntax::parseModule(file, arena_, names_, diags_));
        if (modules_.back() != nullptr && (in.library || in.name.starts_with("<std>/"))) {
            modules_.back()->library = true;
        }
    }
    if (diags_.hasErrors()) {
        // Like `quartz check`, report only syntax errors, but still resolve what parsed for navigation.
        Diagnostics hidden(sources_);
        program_ = sema::analyze(modules_, names_, arena_, hidden);
    } else {
        program_ = sema::analyze(modules_, names_, arena_, diags_);
        if (program_ && !diags_.hasErrors()) {
            // Lowering takes the type table; hand it back (a superset of sema's ids) for queries.
            if (auto lowered = ir::lower(*program_, sources_, diags_)) {
                program_->types = std::move(lowered->types);
            }
        }
    }
    millis_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

Analysis::~Analysis() = default;

const Index& Analysis::index() const {
    if (!index_) {
        index_ = std::make_unique<Index>(*this);
    }
    return *index_;
}

const LineIndex& Analysis::lines(std::uint32_t file) const {
    lines_.resize(inputs_.size());
    if (!lines_[file]) {
        lines_[file] = std::make_unique<LineIndex>(inputs_[file].text);
    }
    return *lines_[file];
}

std::optional<std::uint32_t> Analysis::fileOf(std::string_view uri) const {
    for (std::uint32_t i = 0; i < inputs_.size(); ++i) {
        if (!uri.empty() && inputs_[i].uri == uri) {
            return i;
        }
    }
    return std::nullopt;
}

std::string Analysis::typeName(std::uint32_t t) const {
    if (!program_ || t >= program_->types.size()) {
        return "?";
    }
    return program_->types.name(t);
}

std::string Workspace::folderFor(const std::string& uri) const {
    std::string path = uriToPath(uri);
    if (path.empty()) {
        return std::string(kUriFolder) + uri;
    }
    if (qilnApiFor(path)) {
        return std::string(kQilnFolder) + path;
    }
    return fs::path(path).parent_path().string();
}

std::string Workspace::folderOf(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it != docs_.end() ? it->second.folder : folderFor(uri);
}

void Workspace::open(const std::string& uri, std::string text, std::int64_t version) {
    Document& d = docs_[uri];
    d.uri = uri;
    d.path = uriToPath(uri);
    d.text = std::move(text);
    d.version = version;
    d.folder = folderFor(uri);
    folders_[d.folder].dirty = true;
}

void Workspace::change(const std::string& uri, std::string text, std::int64_t version) {
    auto it = docs_.find(uri);
    if (it == docs_.end() || it->second.folder != folderFor(uri)) {
        // A build script becomes one once Qiln has written the workspace's `.qiln/api`.
        if (it != docs_.end()) {
            close(uri);
        }
        open(uri, std::move(text), version);
        return;
    }
    it->second.text = std::move(text);
    it->second.version = version;
    folders_[it->second.folder].dirty = true;
}

void Workspace::close(const std::string& uri) {
    docs_.erase(uri);
    std::string folder = folderOf(uri);
    if (documentsIn(folder).empty()) {
        folders_.erase(folder);
    } else {
        folders_[folder].dirty = true;
    }
}

const Document* Workspace::document(const std::string& uri) const {
    auto it = docs_.find(uri);
    return it == docs_.end() ? nullptr : &it->second;
}

std::vector<std::string> Workspace::documentsIn(const std::string& folder) const {
    std::vector<std::string> out;
    for (const auto& [uri, doc] : docs_) {
        if (folderOf(uri) == folder) {
            out.push_back(uri);
        }
    }
    return out;
}

std::vector<std::string> Workspace::dirtyFolders() const {
    std::vector<std::string> out;
    for (const auto& [name, f] : folders_) {
        if (f.dirty) {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<std::string> Workspace::folders() const {
    std::vector<std::string> out;
    for (const auto& [name, f] : folders_) {
        out.push_back(name);
    }
    return out;
}

bool Workspace::isDirty(const std::string& folder) const {
    auto it = folders_.find(folder);
    return it != folders_.end() && (it->second.dirty || !it->second.analysis);
}

std::vector<Analysis::Input> Workspace::inputsFor(const std::string& folder) const {
    std::vector<Analysis::Input> inputs;
    if (folder.starts_with(kQilnFolder)) {
        // Like Qiln: the script with the build API and the plugins' APIs, without the CLI's host interface.
        std::string path = folder.substr(kQilnFolder.size());
        for (const std::string& uri : documentsIn(folder)) {
            const Document& d = docs_.at(uri);
            inputs.push_back({d.uri, d.path, d.text});
        }
        if (auto api = qilnApiFor(path)) {
            for (const fs::path& p : qilnApiSources(*api)) {
                inputs.push_back({pathToUri(p.string()), p.string(), readFile(p), true});
            }
        }
        for (const auto& [name, text] : stdlib::sources()) {
            inputs.push_back({{}, std::string(name), std::string(text)});
        }
        return inputs;
    }
    std::set<std::string> openPaths;
    std::set<std::string> packages;
    for (const std::string& uri : documentsIn(folder)) {
        const Document& d = docs_.at(uri);
        inputs.push_back({d.uri, d.path.empty() ? d.uri : d.path, d.text});
        openPaths.insert(d.path);
        std::string pkg = d.path.ends_with(".qz") ? packageOf(d.text) : std::string{};
        if (!pkg.empty()) {
            packages.insert(pkg);
        }
    }
    if (!folder.starts_with(kUriFolder)) {
        std::vector<fs::path> disk;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(folder, ec)) {
            std::string p = e.path().string();
            if (isSource(p) && !openPaths.contains(p) && e.is_regular_file(ec)) {
                disk.push_back(e.path());
            }
        }
        std::sort(disk.begin(), disk.end());
        for (const fs::path& p : disk) {
            std::string text = readFile(p);
            bool iface = p.extension() == ".qzi";
            // With a package open, the folder's other packages come too so that their imports resolve.
            if (iface || (!packages.empty() && !packageOf(text).empty())) {
                inputs.push_back({pathToUri(p.string()), p.string(), std::move(text)});
            }
        }
    }
    inputs.push_back({{}, std::string(cli::kHostInterfaceName), std::string(cli::kHostInterface)});
    for (const auto& [name, text] : stdlib::sources()) {
        inputs.push_back({{}, std::string(name), std::string(text)});
    }
    return inputs;
}

Analysis* Workspace::analyzeFolder(const std::string& folder) {
    Folder& f = folders_[folder];
    if (f.dirty || !f.analysis) {
        f.dirty = false;
        f.analysis.reset();
        f.analysis = std::make_unique<Analysis>(inputsFor(folder));
    }
    return f.analysis.get();
}

Analysis* Workspace::analysisFor(const std::string& uri) {
    if (!docs_.contains(uri)) {
        return nullptr;
    }
    return analyzeFolder(folderOf(uri));
}

} // namespace quartz::lsp
