#pragma once

#include <filesystem>
#include <string>

namespace quartz::support {

/// A shared library loaded at run time; the platform's dlopen or LoadLibrary.
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    DynamicLibrary(DynamicLibrary&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }

    DynamicLibrary& operator=(DynamicLibrary&& o) noexcept;
    ~DynamicLibrary();

    /// Empty on failure, with the reason in `error`.
    static DynamicLibrary open(const std::string& path, std::string& error);
    void* symbol(const char* name) const;

    explicit operator bool() const { return handle_ != nullptr; }

    /// Keeps the library loaded until the process exits, for code that may still run during static teardown.
    void keepLoaded() { handle_ = nullptr; }

private:
    void* handle_ = nullptr;
};

/// File name of the native compiler component on this platform.
const char* compilerLibraryName();
/// The running executable, or an empty path when the platform cannot tell.
std::filesystem::path executablePath();
/// Lowers the calling thread's priority so background compilation does not compete with the host.
void lowerThreadPriority();
unsigned long processId();

} // namespace quartz::support
