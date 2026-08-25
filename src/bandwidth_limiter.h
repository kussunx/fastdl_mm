#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class BandwidthLimiter {
private:
    struct Bucket;
    struct Client;

public:
    class Lease {
    public:
        Lease() = default;

    private:
        friend class BandwidthLimiter;
        explicit Lease(std::shared_ptr<Client> client) : client_(std::move(client)) {}
        std::shared_ptr<Client> client_;
    };

    void configure(double globalMbps, double perIpMbps);
    void stop();
    Lease attach(const std::string& ip);

    // Returns the number of bytes granted, or zero when shutdown cancelled the wait.
    std::size_t acquire(const Lease& lease, std::size_t desired);

private:
    struct Bucket {
        std::uint64_t bytesPerSecond = 0;
        std::int64_t burstNanoseconds = 0;
        std::atomic<std::int64_t> theoreticalArrival{0};
    };

    struct Client {
        Bucket bucket;
    };

    static std::uint64_t rateFromMbps(double mbps);
    static std::int64_t nowNanoseconds();
    static std::int64_t durationFor(std::size_t bytes, std::uint64_t rate);
    static void configureBucket(Bucket& bucket, std::uint64_t rate,
        std::size_t quantum, std::int64_t now);
    static std::int64_t reserve(Bucket& bucket, std::size_t bytes, std::int64_t now);
    void cleanupClients();

    static constexpr std::size_t kMaximumTrackedClients = 256;

    Bucket global_;
    std::uint64_t perIpRate_ = 0;
    std::size_t quantum_ = 64 * 1024;
    std::atomic<bool> stopping_{true};
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::mutex clientsMutex_;
    std::unordered_map<std::string, std::weak_ptr<Client>> clients_;
    std::shared_ptr<Client> overflowClient_;
    std::size_t attachmentsSinceCleanup_ = 0;
};
