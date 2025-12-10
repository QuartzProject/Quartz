#include "Fmt.hpp"

#include "format/Format.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

bool isSource(const fs::path& p) { return p.extension() == ".qz" || p.extension() == ".qzi"; }

void collect(const fs::path& p, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        out.push_back(p);
        return;
    }
    for (const auto& e : fs::recursive_directory_iterator(p, ec)) {
        if (e.is_regular_file() && isSource(e.path())) {
            out.push_back(e.path());
        }
    }
}

int formatStdin() {
    std::stringstream ss;
    ss << std::cin.rdbuf();
    auto out = quartz::format::formatSource("<stdin>", ss.str());
    if (!out) {
        std::fputs(out.error().c_str(), stderr);
        return 1;
    }
    std::fwrite(out->data(), 1, out->size(), stdout);
    return 0;
}

} // namespace

int runFmt(int argc, char** argv) {
    bool check = false;
    std::vector<fs::path> files;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--check") {
            check = true;
        } else if (arg == "-") {
            return formatStdin();
        } else {
            collect(arg, files);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: quartz fmt [--check] <file|dir>... | quartz fmt -\n");
        return 2;
    }
    int status = 0;
    for (const fs::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            status = 1;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        auto out = quartz::format::formatSource(path.string(), text);
        if (!out) {
            std::fputs(out.error().c_str(), stderr);
            status = 1;
            continue;
        }
        if (*out == text) {
            continue;
        }
        if (check) {
            std::printf("%s\n", path.c_str());
            status = 1;
            continue;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << *out;
        if (!file) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            status = 1;
        }
    }
    return status;
}
