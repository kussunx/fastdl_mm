#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

enum class TransferOutcome {
    Completed,
    Aborted,
    TimedOut,
    Failed
};

struct TransferStats {
    std::uint64_t requests = 0;
    std::uint64_t active = 0;
    std::uint64_t completed = 0;
    std::uint64_t aborted = 0;
    std::uint64_t timedOut = 0;
    std::uint64_t failed = 0;
    std::uint64_t transferredBytes = 0;
    double throughputBitsPerSecond = 0.0;
};

class TransferMetrics {
public:
    void reset();
    void requestStarted();
    void transferStarted();
    void bytesSupplied(std::uint64_t bytes);
    void failure();
    void transferFinished(TransferOutcome outcome);
    TransferStats snapshot() const;

private:
    using AtomicCounter = std::atomic<std::uint64_t>;
    static constexpr std::size_t kWindowSeconds = 10;

    struct Bucket {
        std::atomic<std::int64_t> second{-1};
        AtomicCounter bytes{0};
    };

    static std::int64_t currentSecond();

    AtomicCounter requests_{0};
    AtomicCounter active_{0};
    AtomicCounter completed_{0};
    AtomicCounter aborted_{0};
    AtomicCounter timedOut_{0};
    AtomicCounter failed_{0};
    AtomicCounter transferredBytes_{0};
    std::array<Bucket, kWindowSeconds> buckets_{};
    std::atomic<std::int64_t> startedSecond_{-1};
    mutable std::mutex bucketResetMutex_;
};
