#pragma once

#include "path_resolver.h"
#include "secure_file.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

struct CompressionSource {
    std::filesystem::path path;
    std::string extension;
    OpenedFileInfo info;
};

struct CachedRepresentation {
    std::filesystem::path path;
    std::uint64_t size = 0;
};

struct CompressionStats {
    bool running = false;
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t queued = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t failures = 0;
};

class CompressionCache {
public:
    CompressionCache() = default;
    ~CompressionCache();
    CompressionCache(const CompressionCache&) = delete;
    CompressionCache& operator=(const CompressionCache&) = delete;

    bool start(const std::filesystem::path& cachePath,
        const std::filesystem::path& contentRoot, std::uint64_t maxBytes,
        const PathResolver* resolver, std::uint64_t maxFileBytes, std::string& error);
    void stop();
    bool lookup(const CompressionSource& source, CachedRepresentation& cached);
    void requestBuild();
    CompressionStats stats() const;
    const std::filesystem::path& path() const { return cachePath_; }

private:
    struct Job {
        CompressionSource source;
        std::string identity;
        std::string key;
    };

    static bool eligible(const CompressionSource& source);
    static std::string sourceIdentity(const CompressionSource& source);
    static std::string cacheKey(const std::string& identity);
    static bool containedBy(const std::filesystem::path& root,
        const std::filesystem::path& candidate);
    bool markerMatches(const std::filesystem::path& path,
        const std::string& identity) const;
    void enqueue(const CompressionSource& source,
        const std::string& identity, const std::string& key);
    void run();
    void scanAndBuild();
    bool compress(const Job& job);
    void refreshSizeAndPrune();

    static constexpr std::size_t kMaximumQueuedJobs = 1024;
    std::filesystem::path cachePath_;
    const PathResolver* resolver_ = nullptr;
    std::uint64_t maxFileBytes_ = 0;
    std::uint64_t maxBytes_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::unordered_set<std::string> pending_;
    std::thread worker_;
    bool stopping_ = true;
    bool buildRequested_ = false;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> entries_{0};
    std::atomic<std::uint64_t> bytes_{0};
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> failures_{0};
};
