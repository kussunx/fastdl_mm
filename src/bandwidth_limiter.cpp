#include "bandwidth_limiter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {
constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::size_t kMinimumQuantum = 1024;
constexpr std::size_t kMaximumQuantum = 16 * 1024;
constexpr std::uint64_t kMaximumBurstBytes = 256 * 1024;
}

std::uint64_t BandwidthLimiter::rateFromMbps(double mbps) {
    if (!std::isfinite(mbps) || mbps <= 0.0) return 0;
    const long double bytes = static_cast<long double>(mbps) * 1000000.0L / 8.0L;
    if (bytes >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(bytes));
}

std::int64_t BandwidthLimiter::nowNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t BandwidthLimiter::durationFor(std::size_t bytes, std::uint64_t rate) {
    if (rate == 0 || bytes == 0) return 0;
    const auto whole = static_cast<std::uint64_t>(bytes) / rate;
    const auto remainder = static_cast<std::uint64_t>(bytes) % rate;
    const auto fractional = remainder == 0 ? 0 :
        (remainder * kNanosecondsPerSecond + rate - 1) / rate;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (whole > maximum / kNanosecondsPerSecond ||
        whole * kNanosecondsPerSecond > maximum - fractional) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(whole * kNanosecondsPerSecond + fractional);
}

void BandwidthLimiter::configureBucket(Bucket& bucket, std::uint64_t rate,
    std::size_t quantum, std::int64_t now) {
    bucket.bytesPerSecond = rate;
    const auto burstBytes = std::max<std::uint64_t>(quantum,
        std::min<std::uint64_t>(kMaximumBurstBytes, rate / 10));
    bucket.burstNanoseconds = durationFor(static_cast<std::size_t>(burstBytes), rate);
    bucket.theoreticalArrival.store(now, std::memory_order_relaxed);
}

void BandwidthLimiter::configure(double globalMbps, double perIpMbps) {
    stop();
    const auto globalRate = rateFromMbps(globalMbps);
    perIpRate_ = rateFromMbps(perIpMbps);
    auto controllingRate = globalRate;
    if (controllingRate == 0 || (perIpRate_ != 0 && perIpRate_ < controllingRate)) {
        controllingRate = perIpRate_;
    }
    quantum_ = controllingRate == 0 ? 64 * 1024 :
        static_cast<std::size_t>(std::clamp<std::uint64_t>(
            controllingRate / 20, kMinimumQuantum, kMaximumQuantum));
    configureBucket(global_, globalRate, quantum_, nowNanoseconds());
    stopping_.store(false, std::memory_order_release);
}

void BandwidthLimiter::stop() {
    stopping_.store(true, std::memory_order_release);
    wake_.notify_all();
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.clear();
    attachmentsSinceCleanup_ = 0;
}

void BandwidthLimiter::cleanupClients() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second.expired()) {
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
    attachmentsSinceCleanup_ = 0;
}

BandwidthLimiter::Lease BandwidthLimiter::attach(const std::string& ip) {
    if (perIpRate_ == 0) return Lease{};
    std::lock_guard<std::mutex> lock(clientsMutex_);
    if (++attachmentsSinceCleanup_ >= 64) cleanupClients();
    const auto existing = clients_.find(ip);
    if (existing != clients_.end()) {
        if (auto client = existing->second.lock()) return Lease(std::move(client));
    }
    auto client = std::make_shared<Client>();
    configureBucket(client->bucket, perIpRate_, quantum_, nowNanoseconds());
    clients_[ip] = client;
    return Lease(std::move(client));
}

std::int64_t BandwidthLimiter::reserve(
    Bucket& bucket, std::size_t bytes, std::int64_t now) {
    if (bucket.bytesPerSecond == 0) return now;
    const auto duration = durationFor(bytes, bucket.bytesPerSecond);
    auto previous = bucket.theoreticalArrival.load(std::memory_order_relaxed);
    for (;;) {
        const auto earliest = previous > bucket.burstNanoseconds
            ? previous - bucket.burstNanoseconds : 0;
        const auto allowed = std::max(now, earliest);
        const auto base = std::max(previous, allowed);
        const auto next = base > std::numeric_limits<std::int64_t>::max() - duration
            ? std::numeric_limits<std::int64_t>::max() : base + duration;
        if (bucket.theoreticalArrival.compare_exchange_weak(previous, next,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
            return allowed;
        }
    }
}

std::size_t BandwidthLimiter::acquire(const Lease& lease, std::size_t desired) {
    if (desired == 0 || stopping_.load(std::memory_order_acquire)) return 0;
    if (global_.bytesPerSecond == 0 && !lease.client_) return desired;

    const auto granted = std::min(desired, quantum_);
    const auto now = nowNanoseconds();
    auto allowed = reserve(global_, granted, now);
    if (lease.client_) {
        allowed = std::max(allowed, reserve(lease.client_->bucket, granted, now));
    }
    if (allowed > now) {
        std::unique_lock<std::mutex> lock(waitMutex_);
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(allowed));
        if (wake_.wait_until(lock, deadline,
                [this] { return stopping_.load(std::memory_order_acquire); })) {
            return 0;
        }
    }
    return stopping_.load(std::memory_order_acquire) ? 0 : granted;
}
