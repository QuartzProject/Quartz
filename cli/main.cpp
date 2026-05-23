#include "Commands.hpp"
#include "Fmt.hpp"
#include "HostInterface.hpp"
#include "Registry.hpp"
#include "dap/Session.hpp"
#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"
#include "vm/Profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

bool hostPrint(quartz::vm::HostCall& call, void*) {
    std::string_view s = static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return true;
}

// `quartz run` is a host like any other: it grants the log capability by binding the standard sink to stderr.
bool hostLog(quartz::vm::HostCall& call, void*) {
    static constexpr const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    auto text = [&](const quartz::vm::Slot& s) { return static_cast<const quartz::vm::Str*>(s.p)->view(); };
    std::string line = std::string("[") + kLevels[std::clamp<std::int64_t>(call.args[0].i, 0, 4)] + "] ";
    line.append(text(call.args[1])).append(": ").append(text(call.args[2]));
    const auto* fields = static_cast<const quartz::vm::List*>(call.args[3].p);
    for (std::uint32_t i = 0; i + 1 < fields->len; i += 2) {
        line.append(" ").append(text(fields->data[i])).append("=").append(text(fields->data[i + 1]));
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: quartz check|ir|run|dis|test <file.qz|file.qzi|package dir>... | <file.qzb> [--entry name]\n"
                 "                [--filter text] (test) [--profile] [--profile-out file.json] (run)\n"
                 "                [--trust <public key hex>]... [--require-signature] [--host-only]\n"
                 "       quartz build <file.qz|file.qzi|package dir>... -o <out.qzb> [--sign <seed file>] "
                 "[--strip-lines] [--host-only]\n"
                 "       quartz interface <package.qzb>\n"
                 "       quartz add <name>[@range] [package dir] [--registry url]\n"
                 "       quartz fetch [package dir] [--registry url] [--update] [--allow-key-change]\n"
                 "       quartz publish [package dir] --registry <dir> --sign <seed file>\n"
                 "       quartz owner <prefix> <public key hex> --registry <dir>\n"
                 "       quartz keygen <seed file>\n"
                 "       quartz new <package name, like org.example.hello>\n"
                 "       quartz doc <file.qz|file.qzi|package dir>... [-o out.md]\n"
                 "       quartz repl\n"
                 "       quartz debug --dap\n"
                 "       quartz fmt [--check] <file|dir>... | quartz fmt -\n");
    return 2;
}

std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::span<const std::uint8_t> asBytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

bool writeFile(const std::string& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() != N * 2) {
        return std::nullopt;
    }
    auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        int hi = digit(text[2 * i]);
        int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return out;
}

int keygen(const char* path) {
    std::random_device rd;
    quartz::ir::SecretSeed seed{};
    for (std::uint8_t& b : seed) {
        b = static_cast<std::uint8_t>(rd());
    }
    if (!writeFile(path, asBytes(toHex(seed) + "\n"))) {
        return 2;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::printf("%s\n", toHex(quartz::ir::publicKey(seed)).c_str());
    return 0;
}

void bindHost(quartz::vm::Sandbox& sb) {
    sb.bind("print", hostPrint);
    sb.bind("int.ticks", hostTicks);
    sb.bind("org.quartzlang.log.emit", hostLog);
}

int runProgram(const std::shared_ptr<const quartz::vm::Program>& program, const std::string& entry,
               const std::vector<quartz::cli::Plugin>& plugins, quartz::vm::Profiler* profiler) {
    quartz::cli::PluginHost host;
    auto started = host.start(plugins, bindHost);
    quartz::vm::Sandbox sb(program);
    bindHost(sb);
    if (profiler != nullptr) {
        profiler->attach(sb);
    }
    if (auto l = started ? host.link(sb) : started; !l) {
        std::fprintf(stderr, "%s\n", l.error().c_str());
        return 3;
    }
    if (auto s = sb.start(); !s) {
        std::fprintf(stderr, "%s\n", s.error().describe().c_str());
        return 3;
    }
    std::uint32_t fn = program->findFunction(entry);
    if (fn == ~0U) {
        std::fprintf(stderr, "no function '%s'\n", entry.c_str());
        return 2;
    }
    auto t = sb.spawn(fn, {});
    std::uint64_t ticks = 0;
    std::uint64_t yields = 0;
    while (t && t->kind != quartz::vm::TaskOutcome::Kind::Done) {
        if (t->kind == quartz::vm::TaskOutcome::Kind::Wait) {
            ticks += t->value.u;
        } else if (t->kind == quartz::vm::TaskOutcome::Kind::Yield) {
            ++yields;
        }
        t = sb.resume(t->task);
    }
    if (!t) {
        std::fprintf(stderr, "%s\n", t.error().describe().c_str());
        return 3;
    }
    if (ticks > 0 || yields > 0) {
        std::fprintf(stderr, "[%llu ticks waited, %llu fuel yields, %zu bytes in use]\n",
                     static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(yields), sb.memoryUsed());
    }
    if (program->functions[fn].result == quartz::ir::kI64) {
        std::printf("%lld\n", static_cast<long long>(t->value.i));
    }
    return 0;
}

struct Args {
    std::vector<quartz::SourceInput> inputs;
    std::string binaryPath;
    std::string entry = "main";
    std::string output;
    std::string seedPath;
    std::vector<quartz::ir::PublicKey> trusted;
    bool requireSignature = false;
    bool lines = true;
    bool hostOnly = false;
    bool profile = false;
    std::string profileOut;
    std::string manifest;
    std::string filter;
    std::filesystem::path packageDir;
    quartz::cli::DependencyOptions deps;
    std::vector<quartz::cli::Plugin> plugins;
};

// Library sources and plugin interfaces from quartz.lock, fetched and verified first.
bool addDependencies(Args& a) {
    auto text = readFile(a.packageDir / "package.toml");
    if (!text) {
        return true;
    }
    auto m = quartz::parseManifest(*text);
    if (!m) {
        std::fprintf(stderr, "%s\n", m.error().c_str());
        return false;
    }
    auto deps = quartz::cli::prepareDependencies(a.packageDir, *m, a.deps);
    if (!deps) {
        std::fprintf(stderr, "%s\n", deps.error().c_str());
        return false;
    }
    a.inputs.insert(a.inputs.end(), deps->inputs.begin(), deps->inputs.end());
    a.plugins.insert(a.plugins.end(), deps->plugins.begin(), deps->plugins.end());
    return true;
}

std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (arg == "--host-only") {
            a.hostOnly = true;
        } else if (arg == "--entry" && hasValue) {
            a.entry = argv[++i];
        } else if (arg == "-o" && hasValue) {
            a.output = argv[++i];
        } else if (arg == "--sign" && hasValue) {
            a.seedPath = argv[++i];
        } else if (arg == "--trust" && hasValue) {
            auto key = fromHex<32>(argv[++i]);
            if (!key) {
                std::fprintf(stderr, "--trust needs a 64 digit hex public key\n");
                return std::nullopt;
            }
            a.trusted.push_back(*key);
        } else if (arg == "--filter" && hasValue) {
            a.filter = argv[++i];
        } else if (arg == "--registry" && hasValue) {
            a.deps.registry = argv[++i];
        } else if (arg == "--allow-key-change") {
            a.deps.allowKeyChange = true;
        } else if (arg == "--require-signature") {
            a.requireSignature = true;
        } else if (arg == "--strip-lines") {
            a.lines = false;
        } else if (arg == "--profile") {
            a.profile = true;
        } else if (arg == "--profile-out" && hasValue) {
            a.profile = true;
            a.profileOut = argv[++i];
        } else if (arg.ends_with(".qzb")) {
            a.binaryPath = argv[i];
        } else if (std::filesystem::is_directory(argv[i])) {
            // A package directory: its package.toml and every .qz and .qzi below it.
            a.packageDir = argv[i];
            std::vector<std::filesystem::path> files;
            for (const auto& e : std::filesystem::recursive_directory_iterator(argv[i])) {
                auto ext = e.path().extension();
                if (e.is_regular_file() && (ext == ".qz" || ext == ".qzi")) {
                    files.push_back(e.path());
                }
                if (e.is_regular_file() && e.path().filename() == "package.toml") {
                    auto text = readFile(e.path());
                    if (!text) {
                        return std::nullopt;
                    }
                    a.manifest = std::move(*text);
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                auto text = readFile(f);
                if (!text) {
                    return std::nullopt;
                }
                a.inputs.push_back({f.string(), std::move(*text)});
            }
        } else {
            auto text = readFile(argv[i]);
            if (!text) {
                return std::nullopt;
            }
            a.inputs.push_back({argv[i], std::move(*text)});
        }
    }
    if (!a.packageDir.empty() && !addDependencies(a)) {
        return std::nullopt;
    }
    return a;
}

int build(Args& a) {
    if (a.output.empty() || a.inputs.empty() || !a.binaryPath.empty()) {
        return usage();
    }
    if (!a.hostOnly) {
        a.inputs.push_back({std::string(quartz::cli::kHostInterfaceName), std::string(quartz::cli::kHostInterface)});
    }
    quartz::CompileOptions opts;
    opts.emitBinary = true;
    opts.binaryLines = a.lines;
    if (!a.manifest.empty()) {
        auto m = quartz::parseManifest(a.manifest);
        if (!m) {
            std::fprintf(stderr, "%s\n", m.error().c_str());
            return 1;
        }
        opts.manifest = a.manifest;
    }
    quartz::CompileOutput out = quartz::compile(a.inputs, opts);
    std::fputs(out.diagnostics.c_str(), stderr);
    if (!out.program) {
        return 1;
    }
    if (!a.seedPath.empty()) {
        auto text = readFile(a.seedPath.c_str());
        auto seed = text ? fromHex<32>(*text) : std::nullopt;
        if (!seed) {
            std::fprintf(stderr, "%s does not hold a 64 digit hex seed\n", a.seedPath.c_str());
            return 2;
        }
        if (auto s = quartz::ir::sign(out.binary, *seed); !s) {
            std::fprintf(stderr, "%s\n", s.error().c_str());
            return 1;
        }
    }
    return writeFile(a.output, out.binary) ? 0 : 2;
}

// `quartz debug --dap`: the program the client launches, compiled with line probes and linked to its plugins.
std::expected<quartz::dap::DebugTarget, std::string> loadForDebug(const std::string& path) {
    std::string tool = "quartz";
    std::string cmd = "debug";
    std::string target = path;
    char* argv[] = {tool.data(), cmd.data(), target.data()};
    auto a = parseArgs(3, argv);
    if (!a) {
        return std::unexpected("cannot load " + path);
    }
    quartz::dap::DebugTarget out;
    if (!a->binaryPath.empty()) {
        auto bytes = readFile(a->binaryPath);
        auto loaded = bytes ? quartz::load(asBytes(*bytes), {}, {.lines = true, .calls = false})
                            : std::unexpected(std::string("cannot read the module"));
        if (!loaded) {
            return std::unexpected(a->binaryPath + ": " + loaded.error());
        }
        out.program = *loaded;
    } else {
        a->inputs.push_back({std::string(quartz::cli::kHostInterfaceName), std::string(quartz::cli::kHostInterface)});
        quartz::CompileOptions opts;
        opts.probes.lines = true;
        quartz::CompileOutput compiled = quartz::compile(a->inputs, opts);
        if (!compiled.program) {
            return std::unexpected(compiled.diagnostics);
        }
        out.program = compiled.program;
        out.diagnostics = std::move(compiled.diagnostics);
    }
    if (!a->plugins.empty()) {
        out.link = [plugins = a->plugins](quartz::vm::Sandbox& client,
                                          const std::function<void(quartz::vm::Sandbox&)>& bind)
            -> std::expected<std::shared_ptr<void>, std::string> {
            auto host = std::make_shared<quartz::cli::PluginHost>();
            if (auto l = host->start(plugins, bind).and_then([&] { return host->link(client); }); !l) {
                return std::unexpected(l.error());
            }
            return std::shared_ptr<void>(host);
        };
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "repl") {
        return runRepl();
    }
    if (argc < 2) {
        return usage();
    }
    std::string_view cmd = argv[1];
    if (cmd == "add") {
        return quartz::cli::addCommand(argc, argv);
    }
    if (cmd == "fetch") {
        return quartz::cli::fetchCommand(argc, argv);
    }
    if (cmd == "publish") {
        return quartz::cli::publishCommand(argc, argv);
    }
    if (cmd == "owner") {
        return quartz::cli::ownerCommand(argc, argv);
    }
    if (argc < 3) {
        return usage();
    }
    if (cmd == "debug" && std::string_view(argv[2]) == "--dap") {
        return quartz::dap::runDebugAdapter(loadForDebug);
    }
    if (cmd == "fmt") {
        return runFmt(argc, argv);
    }
    if (cmd == "keygen") {
        return keygen(argv[2]);
    }
    if (cmd == "new") {
        return newProject(argv[2]);
    }
    auto args = parseArgs(argc, argv);
    if (!args) {
        return 2;
    }
    if (cmd == "build") {
        return build(*args);
    }
    if (cmd == "doc") {
        return runDoc(args->inputs, args->output);
    }
    std::shared_ptr<const quartz::vm::Program> program;
    std::string irText;
    if (!args->binaryPath.empty()) {
        if (!args->inputs.empty()) {
            std::fprintf(stderr, "a .qzb module cannot be mixed with source files\n");
            return 2;
        }
        auto bytes = readFile(args->binaryPath.c_str());
        if (!bytes) {
            return 2;
        }
        quartz::ir::ReadOptions ro;
        ro.trustedKeys = args->trusted;
        ro.requireSignature = args->requireSignature;
        auto loaded = quartz::load(asBytes(*bytes), ro, {.lines = false, .calls = args->profile});
        if (!loaded) {
            std::fprintf(stderr, "%s: %s\n", args->binaryPath.c_str(), loaded.error().c_str());
            return 1;
        }
        program = *loaded;
        if (cmd == "ir") {
            irText = quartz::ir::print(*program->module);
        }
    } else {
        if (!args->hostOnly) {
            args->inputs.push_back(
                {std::string(quartz::cli::kHostInterfaceName), std::string(quartz::cli::kHostInterface)});
        }
        quartz::CompileOptions opts;
        opts.printIr = cmd == "ir";
        opts.probes.calls = args->profile && cmd == "run";
        quartz::CompileOutput out = quartz::compile(args->inputs, opts);
        std::fputs(out.diagnostics.c_str(), stderr);
        if (cmd == "ir" && !out.program) {
            std::fputs(out.irText.c_str(), stdout);
        }
        if (!out.program) {
            return 1;
        }
        program = out.program;
        irText = std::move(out.irText);
    }
    if (cmd == "check") {
        return 0;
    }
    if (cmd == "test") {
        return runTests(program, args->filter, args->plugins);
    }
    if (cmd == "interface") {
        auto m = quartz::parseManifest(program->module->manifest);
        if (!m) {
            std::fprintf(stderr, "this module has no usable package.toml: %s\n", m.error().c_str());
            return 1;
        }
        auto text = quartz::interfaceOf(*program, *m);
        if (!text) {
            std::fprintf(stderr, "%s\n", text.error().c_str());
            return 1;
        }
        std::fputs(text->c_str(), stdout);
        return 0;
    }
    if (cmd == "ir") {
        std::fputs(irText.c_str(), stdout);
        return 0;
    }
    if (cmd == "dis") {
        for (std::uint32_t f = 0; f < program->functions.size(); ++f) {
            std::fputs(quartz::vm::disassemble(*program, f).c_str(), stdout);
        }
        return 0;
    }
    if (cmd != "run") {
        return usage();
    }
    if (!args->profile) {
        return runProgram(program, args->entry, args->plugins, nullptr);
    }
    quartz::vm::Profiler profiler;
    int code = runProgram(program, args->entry, args->plugins, &profiler);
    std::fputs(profiler.report(*program).c_str(), stderr);
    if (!args->profileOut.empty()) {
        std::string json = profiler.speedscope(*program);
        if (!writeFile(args->profileOut, asBytes(json))) {
            return 2;
        }
    }
    return code;
}
