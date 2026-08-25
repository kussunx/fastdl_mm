#include "rate_limiter.h"

std::size_t RateLimiter::trim(
    std::deque<Clock::time_point>& values, Clock::time_point cutoff) {
    std::size_t removed = 0;
    while (!values.empty() && values.front() <= cutoff) {
        values.pop_front();
        ++removed;
    }
    return removed;
}

void RateLimiter::cleanup(Clock::time_point now, bool force) {
    if (!force && nextCleanup_ != Clock::time_point{} && now < nextCleanup_) {
        return;
    }
    const auto stale = now - std::chrono::minutes(15);
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second.lastSeen < stale && it->second.blockedUntil <= now) {
            eventCount_ -= it->second.requests.size() + it->second.denials.size();
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
    nextCleanup_ = now + std::chrono::minutes(10);
}

RateLimiter::Client* RateLimiter::findOrCreate(
    const std::string& ip, Clock::time_point now) {
    const auto existing = clients_.find(ip);
    if (existing != clients_.end()) return &existing->second;
    if (clients_.size() >= kMaximumClients) cleanup(now, true);
    if (clients_.size() >= kMaximumClients) return nullptr;
    return &clients_.emplace(ip, Client{}).first->second;
}

bool RateLimiter::blocked(const std::string& ip) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    const auto it = clients_.find(ip);
    return it != clients_.end() && it->second.blockedUntil > now;
}

bool RateLimiter::requestExceeded(const std::string& ip, unsigned int limit) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    if (limit == 0) return true;
    auto* client = findOrCreate(ip, now);
    if (client == nullptr) return true;
    client->lastSeen = now;
    eventCount_ -= trim(client->requests, now - std::chrono::minutes(1));
    // Rejected requests are not recorded, or a retrying client stays pinned at
    // the limit and can never drain back under it.
    if (client->requests.size() >= limit || eventCount_ >= kMaximumEvents) {
        return true;
    }
    client->requests.push_back(now);
    ++eventCount_;
    return false;
}

void RateLimiter::recordDenial(
    const std::string& ip, unsigned int limit, unsigned int blockSeconds) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    auto* client = findOrCreate(ip, now);
    if (client == nullptr) return;
    client->lastSeen = now;
    eventCount_ -= trim(client->denials, now - std::chrono::minutes(1));
    if (eventCount_ >= kMaximumEvents) {
        eventCount_ -= client->denials.size();
        client->denials.clear();
        client->blockedUntil = now + std::chrono::seconds(blockSeconds);
        return;
    }
    client->denials.push_back(now);
    ++eventCount_;
    if (limit == 0 || client->denials.size() >= limit) {
        client->blockedUntil = now + std::chrono::seconds(blockSeconds);
        eventCount_ -= client->denials.size();
        client->denials.clear();
    }
}

bool RateLimiter::unblock(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clients_.find(ip);
    if (it == clients_.end()) return false;
    eventCount_ -= it->second.requests.size() + it->second.denials.size();
    clients_.erase(it);
    return true;
}

void RateLimiter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
    nextCleanup_ = {};
    eventCount_ = 0;
}
