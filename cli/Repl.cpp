#include "Commands.hpp"
#include "HostInterface.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool hostPrint(quartz::vm::HostCall& call, void*) {
    std::string_view s = static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return true;
}

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

bool startsWithWord(std::string_view text, std::string_view word) {
    return text.starts_with(word) && (text.size() == word.size() || text[word.size()] == ' ');
}

bool isDeclaration(std::string_view t) {
    for (std::string_view w :
         {"fn", "pub", "internal", "private", "struct", "state", "enum", "trait", "impl", "const"}) {
        if (startsWithWord(t, w)) {
            return true;
        }
    }
    return t.starts_with('@') || t.starts_with("suspend fn");
}

// Brackets still open, ignoring those inside strings, so a multi-line declaration can be typed in pieces.
int openBrackets(std::string_view text) {
    int depth = 0;
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '(' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ')' || c == ']') {
            --depth;
        }
    }
    return depth;
}

class Repl {
public:
    // Declarations and imports accumulate; each other input runs once in `__repl`, and `state` survives via reload.
    bool eval(const std::string& input) {
        std::string_view t = input;
        while (!t.empty() && t.front() == ' ') {
            t.remove_prefix(1);
        }
        if (startsWithWord(t, "import")) {
            return declare(imports_ + std::string(t) + "\n", decls_);
        }
        if (startsWithWord(t, "let") || startsWithWord(t, "var")) {
            // A binding at the prompt becomes a constant later inputs can use; its value is computed when used.
            return declare(imports_, decls_ + "const " + std::string(t.substr(4)) + "\n");
        }
        if (isDeclaration(t)) {
            return declare(imports_, decls_ + std::string(t) + "\n");
        }
        std::string asValue = "    print(\"{" + std::string(t) + "}\")\n";
        if (compiles(asValue)) {
            return run(asValue);
        }
        return run("    " + std::string(t) + "\n");
    }

private:
    // Every declared `state` reaches the input under its name in lower camel case: `state Stats` is `stats`.
    static std::string stateParams(const std::string& decls) {
        std::string params;
        for (std::size_t at = decls.find("state "); at != std::string::npos; at = decls.find("state ", at + 6)) {
            if (at != 0 && decls[at - 1] != '\n' && decls[at - 1] != ' ') {
                continue;
            }
            std::size_t end = decls.find_first_of(" {\n", at + 6);
            std::string type = decls.substr(at + 6, end == std::string::npos ? std::string::npos : end - at - 6);
            if (type.empty()) {
                continue;
            }
            std::string name = type;
            name[0] = static_cast<char>(name[0] >= 'A' && name[0] <= 'Z' ? name[0] - 'A' + 'a' : name[0]);
            params += (params.empty() ? "" : ", ") + name + ": " + type;
        }
        return params;
    }

    std::string source(const std::string& imports, const std::string& decls, const std::string& body) const {
        return imports + decls + "\nfn __repl(" + stateParams(decls) + ") {\n" + body + "}\n";
    }

    std::shared_ptr<const quartz::vm::Program> compile(const std::string& text, std::string* diagnostics) {
        quartz::SourceInput in[] = {
            {"<repl>", text}, {std::string(quartz::cli::kHostInterfaceName), std::string(quartz::cli::kHostInterface)}};
        quartz::CompileOutput out = quartz::compile(in);
        if (diagnostics != nullptr) {
            *diagnostics = out.diagnostics;
        }
        return out.program;
    }

    bool compiles(const std::string& body) { return compile(source(imports_, decls_, body), nullptr) != nullptr; }

    bool declare(const std::string& imports, const std::string& decls) {
        std::string diagnostics;
        auto program = compile(source(imports, decls, ""), &diagnostics);
        if (!program) {
            std::fputs(diagnostics.c_str(), stderr);
            return false;
        }
        imports_ = imports;
        decls_ = decls;
        return adopt(program);
    }

    bool run(const std::string& body) {
        std::string diagnostics;
        auto program = compile(source(imports_, decls_, body), &diagnostics);
        if (!program) {
            std::fputs(diagnostics.c_str(), stderr);
            return false;
        }
        if (!adopt(program)) {
            return false;
        }
        auto r = sandbox_->call(program->findFunction("__repl"), {});
        if (!r) {
            std::fprintf(stderr, "%s\n", r.error().describe().c_str());
            return false;
        }
        return true;
    }

    // A new program keeps the old one's state, so `state` values survive between inputs.
    bool adopt(const std::shared_ptr<const quartz::vm::Program>& program) {
        if (sandbox_ == nullptr) {
            sandbox_ = std::make_unique<quartz::vm::Sandbox>(program);
            sandbox_->bind("print", hostPrint);
            sandbox_->bind("int.ticks", hostTicks);
            if (auto s = sandbox_->start(); !s) {
                std::fprintf(stderr, "%s\n", s.error().describe().c_str());
                sandbox_.reset();
                return false;
            }
            return true;
        }
        auto next = sandbox_->reload(program);
        if (!next) {
            std::fprintf(stderr, "%s\n", next.error().describe().c_str());
            return false;
        }
        sandbox_ = std::move(*next);
        return true;
    }

    std::string imports_;
    std::string decls_;
    std::unique_ptr<quartz::vm::Sandbox> sandbox_;
};

} // namespace

int runRepl() {
    bool tty = isatty(STDIN_FILENO) != 0;
    if (tty) {
        std::printf("Quartz REPL: expressions print their value, declarations stay. :quit to leave.\n");
    }
    Repl repl;
    std::string pending;
    std::string line;
    while (true) {
        if (tty) {
            std::printf("%s", pending.empty() ? "qz> " : "... ");
            std::fflush(stdout);
        }
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (pending.empty() && (line == ":quit" || line == ":q")) {
            break;
        }
        pending += line + "\n";
        if (openBrackets(pending) > 0) {
            continue;
        }
        while (!pending.empty() && (pending.back() == '\n' || pending.back() == ' ')) {
            pending.pop_back();
        }
        if (!pending.empty()) {
            repl.eval(pending);
        }
        pending.clear();
    }
    return 0;
}
