#include "Commands.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace {

bool capturePrint(quartz::vm::HostCall& call, void* user) {
    auto* out = static_cast<std::string*>(user);
    *out += static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    *out += '\n';
    return true;
}

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

} // namespace

int runTests(const std::shared_ptr<const quartz::vm::Program>& program, std::string_view filter,
             const std::vector<quartz::cli::Plugin>& plugins) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const quartz::ir::Annotation& a : program->module->annotations) {
        if (a.name != "test") {
            continue;
        }
        const std::string& name = program->functions[a.function].name;
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            continue;
        }
        std::string printed;
        auto bind = [&](quartz::vm::Sandbox& s) {
            s.bind("print", capturePrint, &printed);
            s.bind("int.ticks", hostTicks);
        };
        quartz::cli::PluginHost host;
        quartz::vm::Sandbox sb(program);
        bind(sb);
        if (auto linked = host.start(plugins, bind).and_then([&] { return host.link(sb); }); !linked) {
            std::fprintf(stderr, "%s\n", linked.error().c_str());
            return 1;
        }
        auto start = std::chrono::steady_clock::now();
        auto started = sb.start();
        auto r = started ? sb.call(a.function, {})
                         : std::expected<quartz::vm::Slot, quartz::vm::Trap>(std::unexpected(started.error()));
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (r) {
            ++passed;
            std::printf("ok      %s (%.1f ms)\n", name.c_str(), ms);
            continue;
        }
        ++failed;
        std::printf("FAILED  %s: %s\n", name.c_str(), r.error().describe().c_str());
        if (!printed.empty()) {
            std::printf("%s", printed.c_str());
        }
    }
    if (passed + failed == 0) {
        std::printf("no tests%s\n", filter.empty() ? "" : " match the filter");
        return filter.empty() ? 0 : 1;
    }
    std::printf("\n%zu passed, %zu failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
