#pragma once

/// `quartz fmt [--check] <file|dir>...`: rewrites files in place, or lists the unformatted ones with `--check`.
int runFmt(int argc, char** argv);
