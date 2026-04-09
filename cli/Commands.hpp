#pragma once

#include "quartz/Quartz.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// `quartz test`: runs every `@test` function in its own sandbox; 0 when all pass.
int runTests(const std::shared_ptr<const quartz::vm::Program>& program, std::string_view filter);

/// `quartz doc`: Markdown for the public declarations and their `///` comments, grouped by package.
int runDoc(const std::vector<quartz::SourceInput>& inputs, const std::string& output);

/// `quartz new <package name>`: a folder with package.toml, a source file and a test.
int newProject(std::string_view name);
