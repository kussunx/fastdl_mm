#include "compression_cache.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#endif

namespace {
class ReadDescriptor {
public:
    explicit ReadDescriptor(const std::filesystem::path& path) : value_(openReadFile(path)) {}
    ~ReadDescriptor() { if (value_ >= 0) closeReadFile(value_); }
    int get() const { return value_; }

private:
    int value_;
};

std::string lowerPathPart(const std::filesystem::path& value) {
    auto text = value.u8string();
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return text;
}

std::uint64_t fnv1a(const std::string& value, std::uint64_t seed) {
    auto hash = seed;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void writeLittleEndian(std::ofstream& output, std::uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 24)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
} // namespace

CompressionCache::~CompressionCache() { stop(); }

bool CompressionCache::containedBy(const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() ||
            lowerPathPart(*rootIt) != lowerPathPart(*candidateIt)) return false;
    }
    return true;
}

bool CompressionCache::start(const std::filesystem::path& cachePath,
    const std::filesystem::path& contentRoot, std::uint64_t maxBytes,
    const PathResolver* resolver, std::uint64_t maxFileBytes, std::string& error) {
    try {
        stop();
        std::error_code ec;
        std::filesystem::create_directories(cachePath, ec);
        if (ec) {
            error = "could not create gzip cache: " + ec.message();
            return false;
        }
        const auto canonicalCache = std::filesystem::canonical(cachePath, ec);
        if (ec || !std::filesystem::is_directory(canonicalCache, ec) || ec) {
            error = "gzip cache is not an accessible directory";
            return false;
        }
        const auto canonicalContent = std::filesystem::canonical(contentRoot, ec);
        if (ec || containedBy(canonicalContent, canonicalCache) ||
            containedBy(canonicalCache, canonicalContent)) {
            error = "gzip cache must be outside the FastDL content root";
            return false;
        }

        cachePath_ = canonicalCache;
        resolver_ = resolver;
        maxFileBytes_ = maxFileBytes;
        maxBytes_ = maxBytes;
        entries_.store(0, std::memory_order_relaxed);
        bytes_.store(0, std::memory_order_relaxed);
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        failures_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = false;
            buildRequested_ = false;
        }
        worker_ = std::thread(&CompressionCache::run, this);
        running_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& exception) {
        stop();
        error = std::string("gzip cache startup exception: ") + exception.what();
        return false;
    } catch (...) {
        stop();
        error = "unknown gzip cache startup exception";
        return false;
    }
}

void CompressionCache::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    running_.store(false, std::memory_order_release);
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
    pending_.clear();
    buildRequested_ = false;
    resolver_ = nullptr;
}

bool CompressionCache::eligible(const CompressionSource& source) {
    if (source.info.size < 1024) return false;
    static const char* extensions[] = {
        ".bsp", ".wad", ".mdl", ".spr", ".wav", ".bmp", ".tga", ".txt", ".res"
    };
    return std::find(std::begin(extensions), std::end(extensions), source.extension) !=
        std::end(extensions);
}

std::string CompressionCache::sourceIdentity(const CompressionSource& source) {
    std::ostringstream value;
    value << source.path.generic_u8string() << '\n' << source.info.size << '\n'
          << source.info.modifiedIdentity << '\n' << source.info.fileIdentity << '\n';
    return value.str();
}

std::string CompressionCache::cacheKey(const std::string& identity) {
    const auto first = fnv1a(identity, 14695981039346656037ULL);
    const auto second = fnv1a(identity, 7809847782465536322ULL);
    std::ostringstream value;
    value << std::hex << std::setfill('0') << std::setw(16) << first
          << std::setw(16) << second;
    return value.str();
}

bool CompressionCache::markerMatches(const std::filesystem::path& path,
    const std::string& identity) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec ||
        std::filesystem::file_size(path, ec) != identity.size() || ec) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string stored(identity.size(), '\0');
    input.read(stored.data(), static_cast<std::streamsize>(stored.size()));
    return input && stored == identity;
}

void CompressionCache::enqueue(const CompressionSource& source,
    const std::string& identity, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= kMaximumQueuedJobs ||
        !pending_.insert(key).second) return;
    jobs_.push_back({source, identity, key});
    ready_.notify_one();
}

bool CompressionCache::lookup(
    const CompressionSource& source, CachedRepresentation& cached) {
    if (!running_.load(std::memory_order_acquire) || !eligible(source)) return false;
    const auto identity = sourceIdentity(source);
    const auto key = cacheKey(identity);
    const auto gzipPath = cachePath_ / (key + ".gz");
    const auto metadataPath = cachePath_ / (key + ".meta");
    std::error_code ec;
    if (markerMatches(metadataPath, identity) &&
        std::filesystem::is_regular_file(gzipPath, ec) && !ec) {
        const auto size = std::filesystem::file_size(gzipPath, ec);
        if (!ec && size >= 18) {
            cached.path = gzipPath;
            cached.size = size;
            hits_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    misses_.fetch_add(1, std::memory_order_relaxed);
    if (!markerMatches(cachePath_ / (key + ".skip"), identity)) {
        enqueue(source, identity, key);
    }
    return false;
}

void CompressionCache::requestBuild() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    buildRequested_ = true;
    ready_.notify_one();
}

CompressionStats CompressionCache::stats() const {
    CompressionStats result;
    result.running = running_.load(std::memory_order_acquire);
    result.entries = entries_.load(std::memory_order_relaxed);
    result.bytes = bytes_.load(std::memory_order_relaxed);
    result.hits = hits_.load(std::memory_order_relaxed);
    result.misses = misses_.load(std::memory_order_relaxed);
    result.failures = failures_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    result.queued = jobs_.size();
    return result;
}

void CompressionCache::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    refreshSizeAndPrune();
    for (;;) {
        Job job;
        bool build = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] {
                return stopping_ || buildRequested_ || !jobs_.empty();
            });
            if (stopping_) break;
            if (buildRequested_) {
                buildRequested_ = false;
                build = true;
            } else {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
        }
        if (build) {
            scanAndBuild();
            continue;
        }
        const bool succeeded = compress(job);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(job.key);
            if (!succeeded && !stopping_) failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void CompressionCache::scanAndBuild() {
    if (resolver_ == nullptr) return;
    std::error_code ec;
    const auto root = resolver_->root();
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
        }
        const auto status = it->symlink_status(ec);
        if (ec || !std::filesystem::is_regular_file(status)) continue;
        const auto relative = it->path().lexically_relative(root).generic_u8string();
        const auto resolved = resolver_->resolve(("/" + relative).c_str(), maxFileBytes_);
        if (resolved.status != ResolveStatus::Ok) continue;
        ReadDescriptor descriptor(resolved.path);
        OpenedFileInfo info;
        if (descriptor.get() < 0 || !inspectReadFile(descriptor.get(), resolved.path, info)) {
            continue;
        }
        CompressionSource source{resolved.path, resolved.extension, info};
        if (!eligible(source)) continue;
        const auto identity = sourceIdentity(source);
        const auto key = cacheKey(identity);
        const auto gzipPath = cachePath_ / (key + ".gz");
        if (markerMatches(cachePath_ / (key + ".meta"), identity) &&
            std::filesystem::is_regular_file(gzipPath, ec) && !ec) continue;
        if (markerMatches(cachePath_ / (key + ".skip"), identity)) continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.insert(key).second) continue;
        }
        const Job job{source, identity, key};
        const bool succeeded = compress(job);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(key);
            if (!succeeded && !stopping_) failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool CompressionCache::compress(const Job& job) {
    ReadDescriptor source(job.source.path);
    OpenedFileInfo before;
    if (source.get() < 0 || !inspectReadFile(source.get(), job.source.path, before) ||
        before.size != job.source.info.size ||
        before.modifiedIdentity != job.source.info.modifiedIdentity ||
        before.fileIdentity != job.source.info.fileIdentity) return false;

    const auto gzipPath = cachePath_ / (job.key + ".gz");
    const auto metadataPath = cachePath_ / (job.key + ".meta");
    const auto skipPath = cachePath_ / (job.key + ".skip");
    const auto gzipTemp = cachePath_ / (job.key + ".gz.tmp");
    const auto metadataTemp = cachePath_ / (job.key + ".meta.tmp");
    std::error_code ec;
    std::filesystem::remove(gzipTemp, ec);
    ec.clear();
    std::filesystem::remove(metadataTemp, ec);

    std::ofstream output(gzipTemp, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const std::array<unsigned char, 10> gzipHeader = {
        0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 255
    };
    output.write(reinterpret_cast<const char*>(gzipHeader.data()), gzipHeader.size());

    mz_stream stream{};
    if (mz_deflateInit2(&stream, MZ_BEST_SPEED, MZ_DEFLATED,
            -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK) {
        output.close();
        std::filesystem::remove(gzipTemp, ec);
        return false;
    }
    bool streamOpen = true;
    const auto finish = [&] {
        if (streamOpen) {
            mz_deflateEnd(&stream);
            streamOpen = false;
        }
    };

    std::array<unsigned char, 64 * 1024> input{};
    std::array<unsigned char, 64 * 1024> compressed{};
    mz_ulong crc = mz_crc32(0, nullptr, 0);
    std::uint64_t offset = 0;
    while (offset < before.size) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                finish();
                output.close();
                std::filesystem::remove(gzipTemp, ec);
                return false;
            }
        }
        const auto wanted = static_cast<std::size_t>(
            std::min<std::uint64_t>(input.size(), before.size - offset));
        const auto read = readFileAt(source.get(), offset, input.data(), wanted);
        if (read <= 0) {
            finish();
            output.close();
            std::filesystem::remove(gzipTemp, ec);
            return false;
        }
        crc = mz_crc32(crc, input.data(), static_cast<std::size_t>(read));
        stream.next_in = input.data();
        stream.avail_in = static_cast<unsigned int>(read);
        while (stream.avail_in != 0) {
            stream.next_out = compressed.data();
            stream.avail_out = static_cast<unsigned int>(compressed.size());
            if (mz_deflate(&stream, MZ_NO_FLUSH) != MZ_OK) {
                finish();
                output.close();
                std::filesystem::remove(gzipTemp, ec);
                return false;
            }
            output.write(reinterpret_cast<const char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size() - stream.avail_out));
        }
        offset += static_cast<std::uint64_t>(read);
        if ((offset & ((256 * 1024) - 1)) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    int deflateStatus = MZ_OK;
    do {
        stream.next_out = compressed.data();
        stream.avail_out = static_cast<unsigned int>(compressed.size());
        deflateStatus = mz_deflate(&stream, MZ_FINISH);
        if (deflateStatus != MZ_OK && deflateStatus != MZ_STREAM_END) break;
        output.write(reinterpret_cast<const char*>(compressed.data()),
            static_cast<std::streamsize>(compressed.size() - stream.avail_out));
    } while (deflateStatus != MZ_STREAM_END);
    finish();
    if (deflateStatus != MZ_STREAM_END) {
        output.close();
        std::filesystem::remove(gzipTemp, ec);
        return false;
    }
    writeLittleEndian(output, static_cast<std::uint32_t>(crc));
    writeLittleEndian(output, static_cast<std::uint32_t>(before.size));
    output.flush();
    if (!output) {
        output.close();
        std::filesystem::remove(gzipTemp, ec);
        return false;
    }
    output.close();

    OpenedFileInfo after;
    if (!inspectReadFile(source.get(), job.source.path, after) ||
        after.size != before.size || after.modifiedIdentity != before.modifiedIdentity ||
        after.fileIdentity != before.fileIdentity) {
        std::filesystem::remove(gzipTemp, ec);
        return false;
    }
    const auto compressedSize = std::filesystem::file_size(gzipTemp, ec);
    if (ec) return false;
    if (compressedSize >= before.size - before.size / 20) {
        std::filesystem::remove(gzipTemp, ec);
        std::filesystem::remove(gzipPath, ec);
        std::filesystem::remove(metadataPath, ec);
        std::ofstream marker(skipPath, std::ios::binary | std::ios::trunc);
        marker.write(job.identity.data(), static_cast<std::streamsize>(job.identity.size()));
        return static_cast<bool>(marker);
    }

    std::ofstream metadata(metadataTemp, std::ios::binary | std::ios::trunc);
    metadata.write(job.identity.data(), static_cast<std::streamsize>(job.identity.size()));
    metadata.flush();
    if (!metadata) {
        metadata.close();
        std::filesystem::remove(gzipTemp, ec);
        std::filesystem::remove(metadataTemp, ec);
        return false;
    }
    metadata.close();

    std::filesystem::remove(gzipPath, ec);
    ec.clear();
    std::filesystem::rename(gzipTemp, gzipPath, ec);
    if (ec) return false;
    std::filesystem::remove(metadataPath, ec);
    ec.clear();
    std::filesystem::rename(metadataTemp, metadataPath, ec);
    if (ec) {
        std::filesystem::remove(gzipPath, ec);
        return false;
    }
    std::filesystem::remove(skipPath, ec);
    refreshSizeAndPrune();
    return true;
}

void CompressionCache::refreshSizeAndPrune() {
    struct Entry {
        std::filesystem::path path;
        std::uint64_t size;
        std::filesystem::file_time_type modified;
    };
    std::vector<Entry> files;
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cachePath_, ec)) {
        if (ec) break;
        if (entry.path().extension() != ".gz" || !entry.is_regular_file(ec) || ec) continue;
        const auto size = entry.file_size(ec);
        if (ec) continue;
        const auto modified = entry.last_write_time(ec);
        if (ec) continue;
        total = size > std::numeric_limits<std::uint64_t>::max() - total
            ? std::numeric_limits<std::uint64_t>::max() : total + size;
        files.push_back({entry.path(), size, modified});
    }
    std::sort(files.begin(), files.end(), [](const Entry& left, const Entry& right) {
        return left.modified < right.modified;
    });
    std::uint64_t count = files.size();
    for (const auto& file : files) {
        if (total <= maxBytes_) break;
        std::error_code removeError;
        if (std::filesystem::remove(file.path, removeError) && !removeError) {
            total -= file.size;
            --count;
            auto marker = file.path;
            marker.replace_extension(".meta");
            std::filesystem::remove(marker, removeError);
        }
    }
    entries_.store(count, std::memory_order_relaxed);
    bytes_.store(total, std::memory_order_relaxed);
}
