#include "Cache.hpp"

#include "vm/NativeAbi.hpp"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/BLAKE3.h>
#include <llvm/Target/TargetMachine.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace quartz::compiler {

namespace {

constexpr char kMagic[4] = {'Q', 'Z', 'N', 'C'};
// Bumped whenever the code generator changes what it emits for the same program.
constexpr std::uint32_t kCacheVersion = 1;
// magic, version, key, object length, object hash
constexpr std::size_t kHeader = 4 + 4 + 32 + 8 + 32;

// The library file's size and modification time: a rebuilt compiler never reuses objects of the old one.
std::string libraryIdentity() {
    static const std::string id = [] {
        Dl_info info{};
        struct stat st{};
        if (dladdr(reinterpret_cast<void*>(&libraryIdentity), &info) == 0 || info.dli_fname == nullptr ||
            stat(info.dli_fname, &st) != 0) {
            return std::string("unknown");
        }
        return std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + "." +
               std::to_string(st.st_mtim.tv_nsec);
    }();
    return id;
}

std::string hex(const CacheKey& key) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : key) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

std::filesystem::path entryPath(const std::string& dir, const CacheKey& key) {
    return std::filesystem::path(dir) / (hex(key) + ".qzo");
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out += static_cast<char>(v >> (8 * i));
    }
}

void put64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out += static_cast<char>(v >> (8 * i));
    }
}

std::uint64_t get(const char* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

void field(llvm::BLAKE3& h, llvm::StringRef s) {
    std::uint64_t n = s.size();
    h.update(llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&n), sizeof n));
    h.update(llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

} // namespace

CacheKey cacheKey(const std::uint8_t (&digest)[32], std::uint32_t function, const llvm::TargetMachine& tm, int opt) {
    llvm::BLAKE3 h;
    field(h, "quartz-native-cache");
    field(h, llvm::StringRef(reinterpret_cast<const char*>(digest), 32));
    std::string nums;
    put32(nums, function);
    put32(nums, vm::kNativeAbi);
    put32(nums, kCacheVersion);
    put32(nums, static_cast<std::uint32_t>(opt));
    field(h, nums);
    field(h, LLVM_VERSION_STRING);
    field(h, libraryIdentity());
    field(h, tm.getTargetTriple().str());
    field(h, tm.getTargetCPU());
    field(h, tm.getTargetFeatureString());
    const char* args = std::getenv("QUARTZ_LLVM_ARGS");
    field(h, args != nullptr ? args : "");
    auto r = h.final();
    CacheKey key{};
    std::memcpy(key.data(), r.data(), key.size());
    return key;
}

std::unique_ptr<llvm::MemoryBuffer> loadObject(const std::string& dir, const CacheKey& key) {
    auto file = llvm::MemoryBuffer::getFile(entryPath(dir, key).string(), false, false);
    if (!file) {
        return nullptr;
    }
    llvm::StringRef bytes = (*file)->getBuffer();
    if (bytes.size() < kHeader || std::memcmp(bytes.data(), kMagic, 4) != 0 ||
        get(bytes.data() + 4, 4) != kCacheVersion || std::memcmp(bytes.data() + 8, key.data(), 32) != 0 ||
        get(bytes.data() + 40, 8) != bytes.size() - kHeader) {
        return nullptr;
    }
    llvm::StringRef object = bytes.substr(kHeader);
    auto sum = llvm::BLAKE3::hash(
        llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(object.data()), object.size()));
    if (std::memcmp(sum.data(), bytes.data() + 48, 32) != 0) {
        return nullptr;
    }
    return llvm::MemoryBuffer::getMemBufferCopy(object, "quartz-cached-object");
}

void storeObject(const std::string& dir, const CacheKey& key, llvm::StringRef object) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string out(kMagic, 4);
    put32(out, kCacheVersion);
    out.append(reinterpret_cast<const char*>(key.data()), key.size());
    put64(out, object.size());
    auto sum = llvm::BLAKE3::hash(
        llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(object.data()), object.size()));
    out.append(reinterpret_cast<const char*>(sum.data()), sum.size());
    out.append(object.data(), object.size());
    std::filesystem::path path = entryPath(dir, key);
    std::filesystem::path tmp = path;
    tmp += ".tmp" + std::to_string(getpid()) + "." + std::to_string(std::random_device{}());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) {
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
    }
}

void dropObject(const std::string& dir, const CacheKey& key) {
    std::error_code ec;
    std::filesystem::remove(entryPath(dir, key), ec);
}

} // namespace quartz::compiler
