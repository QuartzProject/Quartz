#include "support/Platform.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace quartz::support {

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& o) noexcept {
    if (this != &o) {
        this->~DynamicLibrary();
        handle_ = o.handle_;
        o.handle_ = nullptr;
    }
    return *this;
}

#ifdef _WIN32

DynamicLibrary::~DynamicLibrary() {
    if (handle_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(handle_));
    }
}

DynamicLibrary DynamicLibrary::open(const std::string& path, std::string& error) {
    DynamicLibrary lib;
    lib.handle_ = LoadLibraryA(path.c_str());
    if (lib.handle_ == nullptr) {
        error = path + ": LoadLibrary failed with error " + std::to_string(GetLastError());
    }
    return lib;
}

void* DynamicLibrary::symbol(const char* name) const {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
}

const char* compilerLibraryName() { return "quartz_compiler.dll"; }

std::filesystem::path executablePath() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return n == 0 || n >= 32768 ? std::filesystem::path{} : std::filesystem::path(std::wstring(buf, n));
}

void lowerThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

unsigned long processId() { return GetCurrentProcessId(); }

#else

DynamicLibrary::~DynamicLibrary() {
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
}

DynamicLibrary DynamicLibrary::open(const std::string& path, std::string& error) {
    DynamicLibrary lib;
    lib.handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib.handle_ == nullptr) {
        const char* e = dlerror();
        error = e != nullptr ? e : path + ": cannot be loaded";
    }
    return lib;
}

void* DynamicLibrary::symbol(const char* name) const { return dlsym(handle_, name); }

#ifdef __APPLE__
const char* compilerLibraryName() { return "libquartz_compiler.dylib"; }
#else
const char* compilerLibraryName() { return "libquartz_compiler.so"; }
#endif

std::filesystem::path executablePath() {
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : exe;
}

void lowerThreadPriority() {
#ifdef __linux__
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
#endif
}

unsigned long processId() { return static_cast<unsigned long>(getpid()); }

#endif

} // namespace quartz::support
