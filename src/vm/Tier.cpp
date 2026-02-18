#include "vm/Tier.hpp"

#include "support/Platform.hpp"
#include "vm/Bytecode.hpp"

#include "ir/Serialize.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string_view>
#include <thread>

namespace quartz::vm {

namespace {

// Work units per interpreted call on top of the fuel it burns (one unit per loop back edge or call).
constexpr std::uint64_t kCallWeight = 32;
// About 3 ms of interpreter time on the reference machine (D52).
constexpr std::uint64_t kDefaultThreshold = 200'000;

std::uint64_t envNumber(const char* name, std::uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return std::strtoull(v, nullptr, 10);
}

bool logEnabled() {
    static const bool on = std::getenv("QUARTZ_TIER_LOG") != nullptr;
    return on;
}

std::atomic<bool> gExiting{false};

struct Library {
    QcCreateFn create = nullptr;
    QcCompileFn compile = nullptr;
    QcDestroyFn destroy = nullptr;
    std::string error;
    std::string identity; // size and write time of the library file, part of every native cache key (D70)
    int optLevel = 1;
    std::uint64_t threshold = kDefaultThreshold;

    Library() {
        std::vector<std::string> candidates;
        if (const char* p = std::getenv("QUARTZ_COMPILER_LIB"); p != nullptr && *p != '\0') {
            candidates.emplace_back(p);
        } else {
            candidates.emplace_back(support::compilerLibraryName());
            if (auto exe = support::executablePath(); !exe.empty()) {
                candidates.push_back((exe.parent_path() / support::compilerLibraryName()).string());
            }
        }
        support::DynamicLibrary handle;
        for (const std::string& c : candidates) {
            handle = support::DynamicLibrary::open(c, error);
            if (handle) {
                std::error_code ec;
                auto size = std::filesystem::file_size(c, ec);
                auto time = std::filesystem::last_write_time(c, ec);
                if (!ec) {
                    identity = c + ":" + std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
                }
                break;
            }
        }
        if (!handle) {
            return;
        }
        error.clear();
        auto abi = reinterpret_cast<QcAbiFn>(handle.symbol("qc_abi"));
        create = reinterpret_cast<QcCreateFn>(handle.symbol("qc_create"));
        compile = reinterpret_cast<QcCompileFn>(handle.symbol("qc_compile"));
        destroy = reinterpret_cast<QcDestroyFn>(handle.symbol("qc_destroy"));
        handle.keepLoaded();
        if (abi == nullptr || create == nullptr || compile == nullptr || destroy == nullptr || abi() != kNativeAbi) {
            error = std::string(support::compilerLibraryName()) + " does not match this version of Quartz";
            create = nullptr;
            return;
        }
        optLevel = static_cast<int>(envNumber("QUARTZ_OPT", 1));
        threshold = envNumber("QUARTZ_TIER_THRESHOLD", kDefaultThreshold);
        // Runs before the library's own static destructors, so no JIT is torn down after LLVM is gone.
        std::atexit([] { gExiting.store(true); });
    }

    [[nodiscard]] bool ok() const { return create != nullptr; }
};

const Library& library() {
    static const Library lib;
    return lib;
}

struct Job {
    std::shared_ptr<const Program> program;
    std::uint32_t function;
    std::string cacheDir;
};

/// One low priority background thread compiles hot functions (spec 10.1).
class Worker {
public:
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker() = default;

    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            queue_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void push(Job job) {
        {
            std::lock_guard lock(mutex_);
            if (!thread_.joinable()) {
                thread_ = std::thread([this] { loop(); });
            }
            queue_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void loop() {
        support::lowerThreadPriority();
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) {
                    return;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job.program->tier->compile(*job.program, job.function, job.cacheDir);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread thread_;
    bool stop_ = false;
};

Worker& worker() {
    static Worker w;
    return w;
}

} // namespace

TierState::TierState(std::size_t functions)
    : count_(functions), entries_(std::make_unique<std::atomic<NativeEntry>[]>(functions)),
      work_(std::make_unique<std::atomic<std::uint64_t>[]>(functions)),
      status_(std::make_unique<std::atomic<std::uint8_t>[]>(functions)), errors_(functions), stats_(functions) {
    for (std::size_t i = 0; i < functions; ++i) {
        entries_[i].store(nullptr, std::memory_order_relaxed);
        work_[i].store(0, std::memory_order_relaxed);
        status_[i].store(kCold, std::memory_order_relaxed);
    }
}

TierState::~TierState() {
    if (jit_ != nullptr && !gExiting.load()) {
        library().destroy(jit_);
    }
}

void TierState::charge(const std::shared_ptr<const Program>& program, std::uint32_t f, std::uint64_t fuelUsed,
                       const std::string& cacheDir) {
    if (status_[f].load(std::memory_order_relaxed) != kCold) {
        return;
    }
    std::uint64_t units = fuelUsed + kCallWeight;
    if (work_[f].fetch_add(units, std::memory_order_relaxed) + units < library().threshold) {
        return;
    }
    std::uint8_t expected = kCold;
    if (status_[f].compare_exchange_strong(expected, kQueued)) {
        worker().push({program, f, cacheDir});
    }
}

NativeEntry TierState::compile(const Program& program, std::uint32_t f, const std::string& cacheDir) {
    std::lock_guard lock(mutex_);
    if (NativeEntry e = entries_[f].load(std::memory_order_acquire)) {
        return e;
    }
    if (status_[f].load() == kFailed) {
        return nullptr;
    }
    const Library& lib = library();
    char err[512] = {};
    if (jit_ == nullptr) {
        std::span<const NativeSymbol> rt = nativeRuntimeSymbols();
        jit_ = lib.create(rt.data(), static_cast<std::uint32_t>(rt.size()), lib.optLevel, err, sizeof err);
        if (jit_ == nullptr) {
            errors_[f] = err;
            status_[f].store(kFailed);
            return nullptr;
        }
    }
    NativeEntry e = nullptr;
    NativeCompileStats st{};
    NativeCacheRequest cache{cacheDir.c_str(), {}};
    if (!cacheDir.empty()) {
        if (!digested_) {
            programDigest(program, digest_);
            digested_ = true;
        }
        std::memcpy(cache.digest, digest_, sizeof digest_);
    }
    if (lib.compile(jit_, &program, f, cacheDir.empty() ? nullptr : &cache, &e, &st, err, sizeof err) != 0 ||
        e == nullptr) {
        errors_[f] = err;
        status_[f].store(kFailed);
        if (logEnabled()) {
            std::fprintf(stderr, "quartz: cannot compile '%s': %s\n", program.functions[f].name.c_str(), err);
        }
        return nullptr;
    }
    stats_[f] = st;
    entries_[f].store(e, std::memory_order_release);
    status_[f].store(kReady);
    if (logEnabled()) {
        std::fprintf(
            stderr, "quartz: compiled '%s' (%u functions, %u IR instructions) in %.2f ms (lower %.2f, opt %.2f)%s\n",
            program.functions[f].name.c_str(), st.functions, st.irInstructions, st.totalMs, st.lowerMs, st.optimizeMs,
            st.cache == kNativeCacheHit ? " from the cache" : st.cache == kNativeCacheStored ? ", cached" : "");
    }
    return e;
}

std::string TierState::error(std::uint32_t f) const {
    std::lock_guard lock(mutex_);
    return f < count_ ? errors_[f] : std::string{};
}

NativeCompileStats TierState::stats(std::uint32_t f) const {
    std::lock_guard lock(mutex_);
    return f < count_ ? stats_[f] : NativeCompileStats{};
}

void programDigest(const Program& program, std::uint8_t (&out)[32]) {
    std::vector<std::uint8_t> bytes = ir::encodePayload(*program.module);
    auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(static_cast<std::uint32_t>(program.functions.size()));
    for (const CompiledFunction& f : program.functions) {
        put(f.paramSlots);
        put(f.frameSlots);
        put(f.resultSlots);
        put(f.argBase);
        put(static_cast<std::uint32_t>(f.code.size()));
        put(static_cast<std::uint32_t>(f.loopLines.size()));
        for (const auto& [pc, line] : f.loopLines) {
            put(pc);
            put(line);
        }
    }
    put(static_cast<std::uint32_t>(program.importResultSlots.size()));
    for (std::uint32_t s : program.importResultSlots) {
        put(s);
    }
    bytes.insert(bytes.end(), library().identity.begin(), library().identity.end());
    ir::ContentHash h = ir::contentHash(bytes);
    std::memcpy(out, h.data(), sizeof out);
}

std::string resolveCacheDir(const std::string& requested) {
    if (!requested.empty()) {
        return requested;
    }
    const char* v = std::getenv("QUARTZ_NATIVE_CACHE");
    return v != nullptr ? v : "";
}

std::shared_ptr<TierState> makeTierState(std::size_t functions) { return std::make_shared<TierState>(functions); }

TierMode resolveTierMode(TierMode requested) {
    TierMode mode = requested;
    if (mode == TierMode::Default) {
        const char* v = std::getenv("QUARTZ_TIER");
        std::string_view s = v != nullptr ? v : "";
        mode = s == "interp" ? TierMode::Interpreter : s == "native" ? TierMode::Native : TierMode::Auto;
    }
    if (mode != TierMode::Interpreter && !library().ok()) {
        mode = TierMode::Interpreter;
    }
    return mode;
}

std::string nativeCompilerError() { return library().error; }

} // namespace quartz::vm
