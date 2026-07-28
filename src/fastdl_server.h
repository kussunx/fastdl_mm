#pragma once

#include "config.h"
#include "logger.h"
#include "path_resolver.h"
#include "rate_limiter.h"

#include <microhttpd.h>

#include <cstdint>
#include <string>

// Populated during dispatch, logged once the connection finishes so the line
// records the transfer outcome rather than the moment the response was queued.
struct RequestState {
    bool handled = false;
    unsigned int status = 0;
    std::uint64_t size = 0;
    std::string ip;
    std::string method;
    std::string url;
    std::string userAgent;
    std::string reason;
};

class FastdlServer {
public:
    FastdlServer() = default;
    ~FastdlServer();
    FastdlServer(const FastdlServer&) = delete;
    FastdlServer& operator=(const FastdlServer&) = delete;

    bool start(const FastdlConfig& config, std::string& error);
    void stop();
    bool running() const { return daemon_ != nullptr; }
    bool unblock(const std::string& ip) { return limiter_.unblock(ip); }
    const FastdlConfig& config() const { return config_; }
    const std::filesystem::path& root() const { return resolver_.root(); }
    bool logging() const { return logging_; }
    // Names the file that would be written when logging is off, so the warning
    // can point at the path that failed.
    std::filesystem::path logPath() const {
        return logging_ ? logger_.pathForToday() : logBase_;
    }

private:
    static int handleRequest(void* cls, MHD_Connection* connection, const char* url,
        const char* method, const char* version, const char* uploadData,
        size_t* uploadDataSize, void** requestContext);
    static void requestCompleted(void* cls, MHD_Connection* connection,
        void** requestContext, MHD_RequestTerminationCode terminationCode);

    int dispatch(MHD_Connection* connection, RequestState& state);
    int respondText(MHD_Connection* connection, RequestState& state, unsigned int status,
        const char* body, const char* reason);
    int serveFile(MHD_Connection* connection, const ResolvedFile& file);
    int deny(MHD_Connection* connection, RequestState& state,
        const char* reason, bool strike);
    void logCompleted(const RequestState& state, MHD_RequestTerminationCode code);
    static std::string clientIp(MHD_Connection* connection);
    static std::string header(MHD_Connection* connection, const char* name);
    static bool steamClient(const std::string& userAgent);
    static const char* contentType(const std::string& extension);
    static const char* terminationName(MHD_RequestTerminationCode code);

    MHD_Daemon* daemon_ = nullptr;
    bool logging_ = false;
    std::filesystem::path logBase_;
    FastdlConfig config_;
    PathResolver resolver_;
    RateLimiter limiter_;
    AsyncLogger logger_;
};
