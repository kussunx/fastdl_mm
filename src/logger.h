#pragma once

#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// One file per local day, named from the base path: "logs/fastdl.log" becomes
// "logs/fastdl-2026-07-27.log". Each rollover removes files older than
// keepDays; keepDays 0 removes nothing.
class AsyncLogger {
public:
    AsyncLogger() = default;
    ~AsyncLogger();
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    bool start(const std::filesystem::path& basePath, unsigned int keepDays);
    void stop();
    void write(std::string line);

    std::filesystem::path pathForToday() const;

private:
    void run();
    std::filesystem::path dailyPath(const std::tm& day) const;
    void purge(const std::tm& today) const;

    static constexpr std::size_t kMaxQueuedLines = 4096;
    std::filesystem::path basePath_;
    unsigned int keepDays_ = 7;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::thread thread_;
    bool stopping_ = false;
    std::size_t dropped_ = 0;
};
