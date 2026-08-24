#include "transfer_metrics.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace {
void saturatingAdd(std::atomic<std::uint64_t>& value, std::uint64_t amount) {
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = amount > std::numeric_limits<std::uint64_t>::max() - current
            ? std::numeric_limits<std::uint64_t>::max() : current + amount;
        if (value.compare_exchange_weak(current, next,
                std::memory_order_relaxed, std::memory_order_relaxed)) return;
    }
}
} // namespace

std::int64_t TransferMetrics::currentSecond() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void TransferMetrics::reset() {
    requests_.store(0, std::memory_order_relaxed);
    active_.store(0, std::memory_order_relaxed);
    completed_.store(0, std::memory_order_relaxed);
    aborted_.store(0, std::memory_order_relaxed);
    timedOut_.store(0, std::memory_order_relaxed);
    failed_.store(0, std::memory_order_relaxed);
    transferredBytes_.store(0, std::memory_order_relaxed);
    for (auto& bucket : buckets_) {
        bucket.bytes.store(0, std::memory_order_relaxed);
        bucket.second.store(-1, std::memory_order_relaxed);
    }
    startedSecond_.store(currentSecond(), std::memory_order_relaxed);
}

void TransferMetrics::requestStarted() {
    saturatingAdd(requests_, 1);
}

void TransferMetrics::transferStarted() {
    saturatingAdd(active_, 1);
}

void TransferMetrics::bytesSupplied(std::uint64_t bytes) {
    if (bytes == 0) return;
    saturatingAdd(transferredBytes_, bytes);

    const auto second = currentSecond();
    auto& bucket = buckets_[static_cast<std::size_t>(second) % buckets_.size()];
    if (bucket.second.load(std::memory_order_acquire) != second) {
        std::lock_guard<std::mutex> lock(bucketResetMutex_);
        if (bucket.second.load(std::memory_order_relaxed) != second) {
            bucket.bytes.store(0, std::memory_order_relaxed);
            bucket.second.store(second, std::memory_order_release);
        }
    }
    saturatingAdd(bucket.bytes, bytes);
}

void TransferMetrics::failure() {
    saturatingAdd(failed_, 1);
}

void TransferMetrics::transferFinished(TransferOutcome outcome) {
    active_.fetch_sub(1, std::memory_order_relaxed);
    switch (outcome) {
        case TransferOutcome::Completed:
            saturatingAdd(completed_, 1);
            break;
        case TransferOutcome::Aborted:
            saturatingAdd(aborted_, 1);
            break;
        case TransferOutcome::TimedOut:
            saturatingAdd(timedOut_, 1);
            break;
        case TransferOutcome::Failed:
            failure();
            break;
    }
}

TransferStats TransferMetrics::snapshot() const {
    TransferStats result;
    result.requests = requests_.load(std::memory_order_relaxed);
    result.active = active_.load(std::memory_order_relaxed);
    result.completed = completed_.load(std::memory_order_relaxed);
    result.aborted = aborted_.load(std::memory_order_relaxed);
    result.timedOut = timedOut_.load(std::memory_order_relaxed);
    result.failed = failed_.load(std::memory_order_relaxed);
    result.transferredBytes = transferredBytes_.load(std::memory_order_relaxed);

    const auto now = currentSecond();
    std::uint64_t recentBytes = 0;
    for (const auto& bucket : buckets_) {
        const auto second = bucket.second.load(std::memory_order_acquire);
        if (second <= now && second > now - static_cast<std::int64_t>(kWindowSeconds)) {
            const auto bytes = bucket.bytes.load(std::memory_order_relaxed);
            recentBytes = bytes > std::numeric_limits<std::uint64_t>::max() - recentBytes
                ? std::numeric_limits<std::uint64_t>::max() : recentBytes + bytes;
        }
    }
    const auto started = startedSecond_.load(std::memory_order_relaxed);
    const auto elapsed = started < 0 ? 1 : std::max<std::int64_t>(1, now - started + 1);
    const auto window = std::min<std::int64_t>(kWindowSeconds, elapsed);
    result.throughputBitsPerSecond =
        static_cast<double>(recentBytes) * 8.0 / static_cast<double>(window);
    return result;
}
