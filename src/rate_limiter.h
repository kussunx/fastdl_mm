#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

class RateLimiter {
public:
    bool blocked(const std::string& ip);
    bool requestExceeded(const std::string& ip, unsigned int limit);
    void recordDenial(const std::string& ip, unsigned int limit, unsigned int blockSeconds);
    bool unblock(const std::string& ip);
    void clear();

private:
    using Clock = std::chrono::steady_clock;
    struct Client {
        std::deque<Clock::time_point> requests;
        std::deque<Clock::time_point> denials;
        Clock::time_point blockedUntil{};
        Clock::time_point lastSeen{};
    };

    static std::size_t trim(
        std::deque<Clock::time_point>& values, Clock::time_point cutoff);
    void cleanup(Clock::time_point now, bool force = false);
    Client* findOrCreate(const std::string& ip, Clock::time_point now);

    static constexpr std::size_t kMaximumClients = 1024;
    static constexpr std::size_t kMaximumEvents = 131072;

    std::mutex mutex_;
    std::unordered_map<std::string, Client> clients_;
    Clock::time_point nextCleanup_{};
    std::size_t eventCount_ = 0;
};
