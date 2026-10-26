#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "lsp/Json.hpp"
#include "lsp/Text.hpp"

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

std::vector<Json> runSession(std::string_view name, const std::string& input) {
    fs::path dir = fs::path(QUARTZ_TEST_DIR) / name;
    fs::create_directories(dir);
    std::ofstream(dir / "in", std::ios::binary) << input;
    std::string cmd = std::string("\"") + QUARTZ_LSP_PATH + "\" < \"" + (dir / "in").string() + "\" > \"" +
                      (dir / "out").string() + "\" 2> \"" + (dir / "err").string() + "\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::stringstream ss;
    ss << std::ifstream(dir / "out", std::ios::binary).rdbuf();
    return readFrames(ss.str());
}

Json openDocument(std::string_view uri, std::string_view text) {
    Json doc = Json::object().set("uri", uri).set("languageId", "quartz").set("version", 1).set("text", text);
    return Json::object()
        .set("jsonrpc", "2.0")
        .set("method", "textDocument/didOpen")
        .set("params", Json::object().set("textDocument", doc));
}

Json at(std::string_view uri, int line, int character) {
    return Json::object().set("textDocument", Json::object().set("uri", uri)).set("position", at(line, character));
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

    std::vector<Json> msgs = runSession("lsp-session", input);

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

constexpr std::string_view kToolsUri = "untitled:tools";

constexpr std::string_view kTools = "package org.example.tools\n"
                                    "\n"
                                    "import org.quartzlang.data\n"
                                    "\n"
                                    "fn root(x: float) -> float {\n"
                                    "    return sqrt(x)\n"
                                    "}\n"
                                    "\n"
                                    "fn scale(value: float, factor: float = 2.0) -> float {\n"
                                    "    let twice = root(value) * factor\n"
                                    "    return twice\n"
                                    "}\n"
                                    "\n"
                                    "fn main() -> float {\n"
                                    "    return scale(1.0, 3.0)\n"
                                    "}\n";

TEST_CASE("scripted session: imports and navigation") {
    std::string input;
    input += frame(request(1, "initialize", Json::object().set("capabilities", Json::object())));
    input += frame(openDocument(kToolsUri, kTools));
    input += frame(request(2, "textDocument/completion", at(kToolsUri, 5, 14)));
    Json missing =
        Json::object()
            .set("range", Json::object().set("start", at(5, 11)).set("end", at(5, 15)))
            .set("message", "'sqrt' is in org.quartzlang.math; import it with 'import org.quartzlang.math.sqrt'");
    input += frame(request(3, "textDocument/codeAction",
                           Json::object()
                               .set("textDocument", Json::object().set("uri", kToolsUri))
                               .set("range", Json::object().set("start", at(5, 11)).set("end", at(5, 15)))
                               .set("context", Json::object().set("diagnostics", Json::array().push(missing)))));
    input += frame(request(4, "textDocument/references", at(kToolsUri, 4, 8)));
    Json rename = at(kToolsUri, 5, 16);
    rename.set("newName", "value");
    input += frame(request(5, "textDocument/rename", rename));
    input += frame(request(6, "textDocument/signatureHelp", at(kToolsUri, 14, 22)));
    Json whole = Json::object().set("start", at(0, 0)).set("end", at(16, 0));
    input += frame(request(
        7, "textDocument/inlayHint",
        Json::object().set("textDocument", Json::object().set("uri", kToolsUri)).set("range", std::move(whole))));
    input += frame(request(8, "textDocument/semanticTokens/full",
                           Json::object().set("textDocument", Json::object().set("uri", kToolsUri))));
    input += frame(request(10, "workspace/symbol", Json::object().set("query", "sca")));
    input += frame(request(9, "shutdown", nullptr));
    input += frame(notification("exit", nullptr));
    std::vector<Json> msgs = runSession("lsp-tools", input);

    const Json* completion = response(msgs, 2);
    REQUIRE(completion != nullptr);
    const Json* sqrt = nullptr;
    const Json* parse = nullptr;
    for (const Json& item : (*completion)["result"].items()) {
        sqrt = item["label"].str() == "sqrt" ? &item : sqrt;
        parse = item["label"].str() == "parseJson" ? &item : parse;
    }
    REQUIRE(sqrt != nullptr);
    CHECK((*sqrt)["additionalTextEdits"][0]["newText"].str() == "\nimport org.quartzlang.math.sqrt");
    CHECK((*sqrt)["additionalTextEdits"][0]["range"]["start"]["line"].integer() == 2);
    REQUIRE(parse != nullptr);
    CHECK((*parse)["insertText"].str() == "data.parseJson");
    CHECK(!(*parse).has("additionalTextEdits"));

    const Json* actions = response(msgs, 3);
    REQUIRE(actions != nullptr);
    REQUIRE((*actions)["result"].size() == 1);
    CHECK((*actions)["result"][0]["title"].str() == "Import 'sqrt' from org.quartzlang.math");

    const Json* refs = response(msgs, 4);
    REQUIRE(refs != nullptr);
    CHECK((*refs)["result"].size() == 2);
    const Json* renamed = response(msgs, 5);
    REQUIRE(renamed != nullptr);
    const Json& edits = (*renamed)["result"]["changes"][kToolsUri];
    REQUIRE(edits.size() == 2);
    CHECK(edits[0]["range"]["start"]["character"].integer() == 8);
    CHECK(edits[1]["newText"].str() == "value");

    const Json* help = response(msgs, 6);
    REQUIRE(help != nullptr);
    CHECK((*help)["result"]["signatures"][0]["label"].str() == "fn scale(value: float, factor: float = 2.0) -> float");
    CHECK((*help)["result"]["activeParameter"].integer() == 1);

    const Json* hints = response(msgs, 7);
    REQUIRE(hints != nullptr);
    REQUIRE((*hints)["result"].size() == 3);
    CHECK((*hints)["result"][0]["label"].str() == ": float");
    CHECK((*hints)["result"][2]["label"].str() == "factor:");

    const Json* tokens = response(msgs, 8);
    REQUIRE(tokens != nullptr);
    const Json& data = (*tokens)["result"]["data"];
    REQUIRE(data.size() >= 5);
    CHECK(data[0].integer() == 4);
    CHECK(data[1].integer() == 3);
    CHECK(data[2].integer() == 4);
    const Json* symbols = response(msgs, 10);
    REQUIRE(symbols != nullptr);
    REQUIRE((*symbols)["result"].size() == 1);
    CHECK((*symbols)["result"][0]["name"].str() == "scale");
}

TEST_CASE("scripted session: a build script sees the api that Qiln wrote to .qiln/api") {
    fs::path root = fs::absolute(fs::path(QUARTZ_TEST_DIR) / "lsp-qiln-workspace");
    fs::remove_all(root);
    fs::create_directories(root / ".qiln/api/plugins/pack");
    fs::create_directories(root / "app");
    std::ofstream(root / ".qiln/api/qiln.qzi") << "package org.quartzlang.qiln\n"
                                                  "\n"
                                                  "@handle\n"
                                                  "struct Project {}\n"
                                                  "\n"
                                                  "impl Project {\n"
                                                  "    fn setGroup(self, value: str)\n"
                                                  "}\n";
    std::ofstream(root / ".qiln/api/plugins/pack/pack.qz") << "package org.example.pack\n"
                                                              "\n"
                                                              "import org.quartzlang.qiln.*\n"
                                                              "\n"
                                                              "pub fn pack(p: Project, level: int = 9) {\n"
                                                              "    let unused = level\n"
                                                              "    p.setGroup(\"packed\")\n"
                                                              "}\n";
    std::string build = "import org.quartzlang.qiln.*\n"
                        "import org.example.pack.pack\n"
                        "\n"
                        "fn build(p: Project) {\n"
                        "    p.setGroup(\"org.example\")\n"
                        "    pack(p)\n"
                        "}\n";
    std::string uri = quartz::lsp::pathToUri((root / "app/build.qz").string());

    std::string input;
    input += frame(request(1, "initialize", Json::object().set("capabilities", Json::object())));
    input += frame(openDocument(uri, build));
    input += frame(request(2, "textDocument/definition", at(uri, 5, 5)));
    input += frame(request(3, "textDocument/hover", at(uri, 4, 8)));
    input += frame(request(4, "shutdown", nullptr));
    input += frame(notification("exit", nullptr));
    std::vector<Json> msgs = runSession("lsp-qiln", input);

    for (const Json& m : msgs) {
        if (m["method"].str() == "textDocument/publishDiagnostics") {
            CHECK(m["params"]["uri"].str() == uri);
            CHECK(m["params"]["diagnostics"].size() == 0);
        }
    }
    const Json* def = response(msgs, 2);
    REQUIRE(def != nullptr);
    CHECK((*def)["result"]["uri"].str() == quartz::lsp::pathToUri((root / ".qiln/api/plugins/pack/pack.qz").string()));
    CHECK((*def)["result"]["range"]["start"]["line"].integer() == 4);
    const Json* hover = response(msgs, 3);
    REQUIRE(hover != nullptr);
    CHECK((*hover)["result"]["contents"]["value"].str().find("Project.setGroup(self, value: str)") !=
          std::string_view::npos);
}
