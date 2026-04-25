#pragma once

#include "Registry.hpp"
#include "quartz/Quartz.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// `quartz test`: runs every `@test` function in its own sandbox; 0 when all pass.
/// Each test also gets fresh sandboxes of the plugins it links to.
int runTests(const std::shared_ptr<const quartz::vm::Program>& program, std::string_view filter,
             const std::vector<quartz::cli::Plugin>& plugins);

/// `quartz doc`: Markdown for the public declarations and their `///` comments, grouped by package.
int runDoc(const std::vector<quartz::SourceInput>& inputs, const std::string& output);

/// `quartz repl`: expressions print their value; declarations, imports and `state` carry over between inputs.
int runRepl();

/// `quartz new <package name>`: a folder with package.toml, a source file and a test.
int newProject(std::string_view name);
