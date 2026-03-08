#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "lsp/Json.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using quartz::lsp::Json;

constexpr std::string_view kUri = "file:///quartz-lsp-test-missing-dir/main.qz";

// `"é😀" + 1` checks UTF-16 columns: 12 bytes but 9 code units before the end of the error.
constexpr std::string_view kSource = "fn add(a: int, b: int) -> int {\n"
                                     "    return a + b\n"
                                     "}\n"
                                     "\n"
                                     "fn main() -> int {\n"
                                     "    let s = \"\xc3\xa9\xf0\x9f\x98\x80\" + 1\n"
                                     "    return add(1,2)\n"
                                     "}\n";

Json at(int line, int character) { return Json::object().set("line", line).set("character", character); }

Json textDocument() { return Json::object().set("uri", kUri); }

std::string frame(const Json& msg) {
    std::string body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

Json request(int id, std::string_view method, Json params) {
    return Json::object().set("jsonrpc", "2.0").set("id", id).set("method", method).set("params", std::move(params));
}

Json notification(std::string_view method, Json params) {
    return Json::object().set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
}

std::vector<Json> readFrames(const std::string& out) {
    std::vector<Json> msgs;
    std::size_t pos = 0;
    while (true) {
        std::size_t header = out.find("Content-Length: ", pos);
        std::size_t body = out.find("\r\n\r\n", pos);
        if (header == std::string::npos || body == std::string::npos) {
            break;
        }
        std::size_t length = std::stoul(out.substr(header + 16, body - header - 16));
        if (auto j = Json::parse(std::string_view(out).substr(body + 4, length))) {
            msgs.push_back(std::move(*j));
        }
        pos = body + 4 + length;
    }
    return msgs;
}

const Json* response(const std::vector<Json>& msgs, int id) {
    for (const Json& m : msgs) {
        if (m["id"].integer(-1) == id) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("scripted session: diagnostics, hover, definition and formatting") {
    Json open = Json::object().set("uri", kUri).set("languageId", "quartz").set("version", 1).set("text", kSource);
    std::string input;
    input += frame(request(1, "initialize", Json::object().set("capabilities", Json::object())));
    input += frame(notification("initialized", Json::object()));
    input += frame(notification("textDocument/didOpen", Json::object().set("textDocument", open)));
    input += frame(request(2, "textDocument/hover",
                           Json::object().set("textDocument", textDocument()).set("position", at(6, 12))));
    input += frame(request(3, "textDocument/definition",
                           Json::object().set("textDocument", textDocument()).set("position", at(6, 12))));
    input += frame(request(4, "textDocument/formatting",
                           Json::object().set("textDocument", textDocument()).set("options", Json::object())));
    input += frame(request(5, "no/such/method", Json::object()));
    input += frame(request(6, "shutdown", nullptr));
    input += frame(notification("exit", nullptr));

    fs::path dir = fs::path(QUARTZ_TEST_DIR) / "lsp-session";
    fs::create_directories(dir);
    std::ofstream(dir / "in", std::ios::binary) << input;
    std::string cmd = std::string("\"") + QUARTZ_LSP_PATH + "\" < \"" + (dir / "in").string() + "\" > \"" +
                      (dir / "out").string() + "\" 2> \"" + (dir / "err").string() + "\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::stringstream ss;
    ss << std::ifstream(dir / "out", std::ios::binary).rdbuf();
    std::vector<Json> msgs = readFrames(ss.str());

    const Json* init = response(msgs, 1);
    REQUIRE(init != nullptr);
    CHECK((*init)["result"]["capabilities"]["hoverProvider"].boolean());

    const Json* diags = nullptr;
    for (const Json& m : msgs) {
        if (m["method"].str() == "textDocument/publishDiagnostics" && m["params"]["uri"].str() == kUri) {
            diags = &m["params"]["diagnostics"];
        }
    }
    REQUIRE(diags != nullptr);
    REQUIRE(diags->size() == 1);
    const Json& d = (*diags)[0];
    CHECK(d["severity"].integer() == 1);
    CHECK(d["message"].str().find("mismatched types 'str' and 'int'") != std::string_view::npos);
    CHECK(d["range"]["start"]["line"].integer() == 5);
    CHECK(d["range"]["start"]["character"].integer() == 12);
    CHECK(d["range"]["end"]["character"].integer() == 21);

    const Json* hover = response(msgs, 2);
    REQUIRE(hover != nullptr);
    CHECK((*hover)["result"]["contents"]["value"].str() == "```quartz\nfn add(a: int, b: int) -> int\n```");

    const Json* def = response(msgs, 3);
    REQUIRE(def != nullptr);
    CHECK((*def)["result"]["uri"].str() == kUri);
    CHECK((*def)["result"]["range"]["start"]["line"].integer() == 0);
    CHECK((*def)["result"]["range"]["start"]["character"].integer() == 3);

    const Json* fmt = response(msgs, 4);
    REQUIRE(fmt != nullptr);
    REQUIRE((*fmt)["result"].size() == 1);
    std::string expected(kSource);
    expected.replace(expected.find("add(1,2)"), 8, "add(1, 2)");
    CHECK((*fmt)["result"][0]["newText"].str() == expected);

    const Json* unknown = response(msgs, 5);
    REQUIRE(unknown != nullptr);
    CHECK((*unknown)["error"]["code"].integer() == -32601);
    CHECK(response(msgs, 6) != nullptr);
}
