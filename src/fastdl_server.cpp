#include "fastdl_server.h"
#include "secure_file.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifdef _WIN32
#define FASTDL_REUSE_OPTIONS MHD_OPTION_LISTENING_ADDRESS_REUSE, 0U,
#else
#define FASTDL_REUSE_OPTIONS
#endif

namespace {
constexpr std::size_t kFileBlockSize = 64 * 1024;

std::string sanitize(const std::string& value, std::size_t limit) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(std::min(value.size(), limit));
    for (const unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            if (out.size() > limit || limit - out.size() < 4) break;
            out += "\\x";
            out += digits[c >> 4];
            out += digits[c & 0x0f];
        } else {
            if (out.size() >= limit) break;
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool boundedCopy(const char* value, std::size_t limit, std::string& result) {
    result.clear();
    if (value == nullptr) return true;
    std::size_t length = 0;
    while (length <= limit && value[length] != '\0') ++length;
    if (length > limit) return false;
    result.assign(value, length);
    return true;
}

std::filesystem::path resolveAgainstBase(
    const std::filesystem::path& base, const std::filesystem::path& path) {
    const std::filesystem::path joined =
        (path.is_absolute() || base.empty()) ? path : base / path;
    auto normalized = joined.lexically_normal();
    normalized.make_preferred();
    return normalized;
}

class UniqueFd {
public:
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) closeReadFile(fd_); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    int get() const { return fd_; }
    void swap(UniqueFd& other) { std::swap(fd_, other.fd_); }
    int release() {
        const int value = fd_;
        fd_ = -1;
        return value;
    }

private:
    int fd_;
};

std::string trimAscii(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

bool parseUnsigned(std::string_view value, std::uint64_t& result) {
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned int>(c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

enum class RangeStatus { None, Valid, Invalid, Unsatisfiable };

struct ByteRange {
    RangeStatus status = RangeStatus::None;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
};

ByteRange parseRange(const std::string& raw, std::uint64_t total) {
    ByteRange result;
    const auto value = trimAscii(raw);
    if (value.empty()) return result;
    if (value.size() < 6 ||
        (value[0] != 'b' && value[0] != 'B') ||
        (value[1] != 'y' && value[1] != 'Y') ||
        (value[2] != 't' && value[2] != 'T') ||
        (value[3] != 'e' && value[3] != 'E') ||
        (value[4] != 's' && value[4] != 'S') || value[5] != '=') {
        result.status = RangeStatus::Invalid;
        return result;
    }
    const std::string_view spec(value.data() + 6, value.size() - 6);
    if (spec.empty() || spec.find(',') != std::string_view::npos) {
        result.status = RangeStatus::Invalid;
        return result;
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
        result.status = RangeStatus::Invalid;
        return result;
    }
    if (total == 0) {
        result.status = RangeStatus::Unsatisfiable;
        return result;
    }

    const auto first = spec.substr(0, dash);
    const auto second = spec.substr(dash + 1);
    if (first.empty()) {
        std::uint64_t suffix = 0;
        if (!parseUnsigned(second, suffix) || suffix == 0) {
            result.status = RangeStatus::Invalid;
            return result;
        }
        result.length = std::min(suffix, total);
        result.start = total - result.length;
        result.status = RangeStatus::Valid;
        return result;
    }

    std::uint64_t start = 0;
    if (!parseUnsigned(first, start)) {
        result.status = RangeStatus::Invalid;
        return result;
    }
    if (start >= total) {
        result.status = RangeStatus::Unsatisfiable;
        return result;
    }
    std::uint64_t end = total - 1;
    if (!second.empty() && (!parseUnsigned(second, end) || end < start)) {
        result.status = RangeStatus::Unsatisfiable;
        return result;
    }
    end = std::min(end, total - 1);
    result.start = start;
    result.length = end - start + 1;
    result.status = RangeStatus::Valid;
    return result;
}

std::string httpDate(std::int64_t seconds) {
    const auto value = static_cast<std::time_t>(seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &value) != 0) return {};
#else
    if (gmtime_r(&value, &utc) == nullptr) return {};
#endif
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::put_time(&utc, "%a, %d %b %Y %H:%M:%S GMT");
    return formatted.str();
}

bool parseHttpDate(const std::string& value, std::int64_t& seconds) {
    std::tm utc{};
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    input >> std::get_time(&utc, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) return false;
#ifdef _WIN32
    const auto converted = _mkgmtime64(&utc);
#else
    const auto converted = timegm(&utc);
#endif
    if (converted < 0) return false;
    seconds = static_cast<std::int64_t>(converted);
    return true;
}

std::string makeEtag(const OpenedFileInfo& file, bool gzip = false) {
    std::ostringstream value;
    value << '"' << std::hex << file.size << '-' << file.modifiedIdentity << '-'
          << file.fileIdentity;
    if (gzip) value << "-gz";
    value << '"';
    return value.str();
}

bool acceptsGzip(const std::string& raw) {
    bool wildcard = false;
    bool wildcardAllowed = false;
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto comma = raw.find(',', start);
        const auto end = comma == std::string::npos ? raw.size() : comma;
        auto item = trimAscii(std::string_view(raw).substr(start, end - start));
        const auto semicolon = item.find(';');
        auto coding = trimAscii(std::string_view(item).substr(0, semicolon));
        std::transform(coding.begin(), coding.end(), coding.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool allowed = true;
        for (auto parameterStart = semicolon;
             parameterStart != std::string::npos && parameterStart < item.size();) {
            ++parameterStart;
            const auto parameterEnd = item.find(';', parameterStart);
            auto parameter = trimAscii(std::string_view(item).substr(parameterStart,
                (parameterEnd == std::string::npos ? item.size() : parameterEnd) -
                    parameterStart));
            std::transform(parameter.begin(), parameter.end(), parameter.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (parameter.rfind("q=", 0) == 0) {
                char* endValue = nullptr;
                const double quality = std::strtod(parameter.c_str() + 2, &endValue);
                const bool validQuality = endValue != parameter.c_str() + 2 &&
                    endValue != nullptr && *endValue == '\0' &&
                    std::isfinite(quality) && quality > 0.0 && quality <= 1.0;
                allowed = allowed && validQuality;
            }
            parameterStart = parameterEnd;
        }
        if (coding == "gzip") return allowed;
        if (coding == "*") {
            wildcard = true;
            wildcardAllowed = allowed;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return wildcard && wildcardAllowed;
}

bool etagMatches(const std::string& raw, const std::string& etag) {
    std::size_t start = 0;
    while (start <= raw.size()) {
        const auto comma = raw.find(',', start);
        const auto end = comma == std::string::npos ? raw.size() : comma;
        auto candidate = trimAscii(std::string_view(raw).substr(start, end - start));
        if (candidate == "*") return true;
        if (candidate.rfind("W/", 0) == 0 || candidate.rfind("w/", 0) == 0) {
            candidate = trimAscii(std::string_view(candidate).substr(2));
        }
        if (candidate == etag) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

bool ifRangeMatches(const std::string& raw, const std::string& etag,
    std::int64_t modifiedSeconds) {
    const auto value = trimAscii(raw);
    if (value.empty()) return true;
    if (value == etag) return true;
    std::int64_t date = 0;
    return parseHttpDate(value, date) && modifiedSeconds <= date;
}

bool addHeader(MHD_Response* response, const char* name, const std::string& value) {
    return MHD_add_response_header(response, name, value.c_str()) == MHD_YES;
}

bool addHeader(MHD_Response* response, const char* name, const char* value) {
    return MHD_add_response_header(response, name, value) == MHD_YES;
}

struct TransferContext {
    int fd = -1;
    std::uint64_t sourceOffset = 0;
    std::uint64_t length = 0;
    RequestState* state = nullptr;
    TransferMetrics* metrics = nullptr;
    BandwidthLimiter* limiter = nullptr;
    BandwidthLimiter::Lease lease;

    ~TransferContext() { if (fd >= 0) closeReadFile(fd); }
};

ssize_t fileReader(void* cls, std::uint64_t pos, char* buf, std::size_t max) {
    try {
        auto* context = static_cast<TransferContext*>(cls);
        if (context == nullptr || context->fd < 0 || pos >= context->length) {
            return static_cast<ssize_t>(MHD_CONTENT_READER_END_OF_STREAM);
        }
        const auto remaining = context->length - pos;
        auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(max)));
        requested = context->limiter->acquire(context->lease, requested);
        if (requested == 0) {
            return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
        }
        if (context->sourceOffset > std::numeric_limits<std::uint64_t>::max() - pos) {
            return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
        }
        const auto absolute = context->sourceOffset + pos;
        const auto count = readFileAt(context->fd, absolute, buf, requested);
        if (count <= 0) return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
        const auto supplied = static_cast<std::uint64_t>(count);
        context->state->bytesSupplied.fetch_add(supplied, std::memory_order_relaxed);
        context->metrics->bytesSupplied(supplied);
        return static_cast<ssize_t>(count);
    } catch (...) {
        return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
    }
}

void fileReaderFree(void* cls) {
    try {
        delete static_cast<TransferContext*>(cls);
    } catch (...) {
    }
}

std::string kilobytes(std::uint64_t bytes) {
    std::ostringstream value;
    value << std::fixed << std::setprecision(1)
          << (static_cast<double>(bytes) / 1024.0) << "kB";
    return value.str();
}
} // namespace

FastdlServer::~FastdlServer() { stop(); }

bool FastdlServer::start(const FastdlConfig& config, std::string& error) {
    try {
        stop();
        config_ = config;
        if (!config_.enabled) return true;
        if (!resolver_.configure(resolveAgainstBase(config_.baseDir, config_.root),
                config_.serveDirs, config_.serveTypes, config_.serveRootTypes, error)) {
            return false;
        }
        compressionError_.clear();
        if (config_.gzip && config_.gzipCache) {
            std::string cacheError;
            if (!compression_.start(resolveAgainstBase(
                    config_.baseDir, config_.gzipCachePath), resolver_.root(),
                    config_.gzipCacheMaxBytes, &resolver_, config_.maxFileBytes,
                    cacheError)) {
                compressionError_ = cacheError;
            }
        }
        logBase_ = resolveAgainstBase(config_.baseDir, config_.logPath);
        logging_ = logger_.start(logBase_, config_.logAgeDays);
        bandwidth_.configure(config_.bandwidthMaxMbps, config_.bandwidthIpMbps);
        metrics_.reset();

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);
        if (config_.bindAddress.empty() || config_.bindAddress == "0.0.0.0" ||
            config_.bindAddress == "*") {
            address.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, config_.bindAddress.c_str(), &address.sin_addr) != 1) {
            logger_.stop();
            bandwidth_.stop();
            compression_.stop();
            logging_ = false;
            error = "invalid IPv4 bind address";
            return false;
        }

        daemon_ = MHD_start_daemon(
            MHD_USE_SELECT_INTERNALLY | MHD_USE_PIPE_FOR_SHUTDOWN,
            config_.port, nullptr, nullptr,
            &FastdlServer::handleRequest, this,
            MHD_OPTION_SOCK_ADDR, &address,
            FASTDL_REUSE_OPTIONS
            MHD_OPTION_THREAD_POOL_SIZE, config_.threads,
            MHD_OPTION_CONNECTION_LIMIT, config_.maxConnections,
            MHD_OPTION_PER_IP_CONNECTION_LIMIT, config_.maxConnectionsPerIp,
            MHD_OPTION_CONNECTION_TIMEOUT, config_.connectionTimeoutSeconds,
            MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<size_t>(64 * 1024),
            MHD_OPTION_NOTIFY_COMPLETED, &FastdlServer::requestCompleted, this,
            MHD_OPTION_END);
        if (daemon_ == nullptr) {
            logger_.stop();
            bandwidth_.stop();
            compression_.stop();
            logging_ = false;
            error = "libmicrohttpd could not bind or start";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        stop();
        error = std::string("startup exception: ") + exception.what();
        return false;
    } catch (...) {
        stop();
        error = "unknown startup exception";
        return false;
    }
}

void FastdlServer::stop() {
    bandwidth_.stop();
    if (daemon_ != nullptr) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
    compression_.stop();
    logger_.stop();
    logging_ = false;
    limiter_.clear();
}

int FastdlServer::handleRequest(void* cls, MHD_Connection* connection, const char* url,
    const char* method, const char*, const char*, size_t* uploadDataSize,
    void** requestContext) {
    try {
        auto* server = static_cast<FastdlServer*>(cls);
        if (server == nullptr || connection == nullptr || uploadDataSize == nullptr ||
            requestContext == nullptr) return MHD_NO;
        if (*requestContext == nullptr) {
            auto fresh = std::make_unique<RequestState>();
            fresh->ip = clientIp(connection);
            if (!boundedCopy(method, 16, fresh->method)) fresh->method = "<overlong>";
            fresh->invalidTarget = !boundedCopy(url, 2048, fresh->url);
            if (fresh->invalidTarget) fresh->url = "<overlong>";
            fresh->userAgent = header(connection, MHD_HTTP_HEADER_USER_AGENT, 200);
            *requestContext = fresh.release();
            return MHD_YES;
        }
        auto* state = static_cast<RequestState*>(*requestContext);
        if (*uploadDataSize != 0) {
            *uploadDataSize = 0;
            return MHD_YES;
        }
        if (state->handled) return MHD_YES;
        state->handled = true;
        return server->dispatch(connection, *state);
    } catch (...) {
        return MHD_NO;
    }
}

void FastdlServer::requestCompleted(void* cls, MHD_Connection*, void** requestContext,
    MHD_RequestTerminationCode terminationCode) {
    if (requestContext == nullptr) return;
    auto* server = static_cast<FastdlServer*>(cls);
    auto* state = static_cast<RequestState*>(*requestContext);
    if (state != nullptr) {
        try {
            if (server != nullptr && state->activeTransfer) {
                TransferOutcome outcome = TransferOutcome::Failed;
                if (terminationCode == MHD_REQUEST_TERMINATED_COMPLETED_OK &&
                    state->bytesSupplied.load(std::memory_order_relaxed) ==
                        state->responseSize) {
                    outcome = TransferOutcome::Completed;
                } else if (terminationCode == MHD_REQUEST_TERMINATED_CLIENT_ABORT) {
                    outcome = TransferOutcome::Aborted;
                } else if (terminationCode == MHD_REQUEST_TERMINATED_TIMEOUT_REACHED) {
                    outcome = TransferOutcome::TimedOut;
                }
                server->metrics_.transferFinished(outcome);
            }
            if (server != nullptr && state->handled) {
                server->logCompleted(*state, terminationCode);
            }
        } catch (...) {
        }
        delete state;
    }
    *requestContext = nullptr;
}

int FastdlServer::dispatch(MHD_Connection* connection, RequestState& state) {
    metrics_.requestStarted();
    if (state.method != MHD_HTTP_METHOD_GET && state.method != MHD_HTTP_METHOD_HEAD) {
        return deny(connection, state, "invalid method", true);
    }
    if (limiter_.blocked(state.ip)) return deny(connection, state, "blocked", false);
    if (config_.steamOnly && !steamClient(state.userAgent)) {
        return deny(connection, state, "non-steam client denied", true);
    }
    if (limiter_.requestExceeded(state.ip, config_.requestsPerMinute)) {
        return deny(connection, state, "rate limit", false);
    }
    if (state.invalidTarget) return deny(connection, state, "invalid path", true);

    const auto file = resolver_.resolve(state.url.c_str(), config_.maxFileBytes);
    switch (file.status) {
        case ResolveStatus::Ok: break;
        case ResolveStatus::NotFound:
        case ResolveStatus::NotAFile:
            return respondText(
                connection, state, MHD_HTTP_NOT_FOUND, "404 Not Found\n", "not found");
        case ResolveStatus::TooLarge:
            return respondText(
                connection, state, MHD_HTTP_FORBIDDEN, "403 Forbidden\n", "file too large");
        case ResolveStatus::IoError:
            metrics_.failure();
            return respondText(connection, state, MHD_HTTP_INTERNAL_SERVER_ERROR,
                "500 Internal Server Error\n", "file error");
        case ResolveStatus::OutsideRoot:
            return deny(connection, state, "outside root", true);
        case ResolveStatus::DirectoryDenied:
            return deny(connection, state, "directory not served", true);
        case ResolveStatus::ExtensionDenied:
            return deny(connection, state, "invalid extension", true);
        case ResolveStatus::InvalidPath:
        default:
            return deny(connection, state, "invalid path", true);
    }

    if (serveFile(connection, state, file) != MHD_YES) {
        metrics_.failure();
        return respondText(connection, state, MHD_HTTP_INTERNAL_SERVER_ERROR,
            "500 Internal Server Error\n", "open or response failure");
    }
    return MHD_YES;
}

int FastdlServer::deny(MHD_Connection* connection, RequestState& state,
    const char* reason, bool strike) {
    if (strike) {
        limiter_.recordDenial(state.ip, config_.denialsPerMinute, config_.blockSeconds);
    }
    return respondText(connection, state, MHD_HTTP_FORBIDDEN, "403 Forbidden\n", reason);
}

int FastdlServer::respondText(MHD_Connection* connection, RequestState& state,
    unsigned int status, const char* body, const char* reason) {
    const auto length = std::strlen(body);
    state.fileResponse = false;
    state.status = status;
    state.responseSize = length;
    state.reason = reason;
    auto* response = MHD_create_response_from_buffer(
        length, const_cast<char*>(body), MHD_RESPMEM_PERSISTENT);
    if (response == nullptr) return MHD_NO;
    if (!addHeader(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8") ||
        !addHeader(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store")) {
        MHD_destroy_response(response);
        return MHD_NO;
    }
    const int result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return result;
}

int FastdlServer::respondRangeError(MHD_Connection* connection, RequestState& state,
    std::uint64_t totalSize) {
    static const char body[] = "416 Range Not Satisfiable\n";
    state.fileResponse = false;
    state.status = MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE;
    state.responseSize = sizeof(body) - 1;
    state.reason = "invalid or unsatisfiable range";
    auto* response = MHD_create_response_from_buffer(
        sizeof(body) - 1, const_cast<char*>(body), MHD_RESPMEM_PERSISTENT);
    if (response == nullptr) return MHD_NO;
    const auto contentRange = "bytes */" + std::to_string(totalSize);
    if (!addHeader(response, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain; charset=utf-8") ||
        !addHeader(response, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store") ||
        !addHeader(response, "Accept-Ranges", "bytes") ||
        !addHeader(response, "Content-Range", contentRange)) {
        MHD_destroy_response(response);
        return MHD_NO;
    }
    const int result = MHD_queue_response(
        connection, MHD_HTTP_REQUESTED_RANGE_NOT_SATISFIABLE, response);
    MHD_destroy_response(response);
    return result;
}

int FastdlServer::serveFile(MHD_Connection* connection, RequestState& state,
    const ResolvedFile& file) {
    UniqueFd descriptor(openReadFile(file.path));
    if (descriptor.get() < 0) return MHD_NO;

    OpenedFileInfo opened;
    if (!inspectReadFile(descriptor.get(), file.path, opened)) return MHD_NO;
    if (opened.size > config_.maxFileBytes) {
        return respondText(
            connection, state, MHD_HTTP_FORBIDDEN, "403 Forbidden\n", "file too large");
    }

    const auto lastModified = httpDate(opened.modifiedSeconds);
    auto range = parseRange(
        header(connection, MHD_HTTP_HEADER_RANGE, 256), opened.size);
    if (range.status == RangeStatus::Invalid || range.status == RangeStatus::Unsatisfiable) {
        return respondRangeError(connection, state, opened.size);
    }
    const auto identityEtag = makeEtag(opened);
    if (range.status == RangeStatus::Valid &&
        !ifRangeMatches(header(connection, MHD_HTTP_HEADER_IF_RANGE, 512), identityEtag,
            opened.modifiedSeconds)) range = {};

    const bool partial = range.status == RangeStatus::Valid;
    bool compressed = false;
    std::uint64_t responseLength = partial ? range.length : opened.size;
    if (!partial && config_.gzip && config_.gzipCache &&
        acceptsGzip(header(connection, MHD_HTTP_HEADER_ACCEPT_ENCODING, 1024))) {
        CompressionSource source{file.path, file.extension, opened};
        CachedRepresentation cached;
        if (compression_.lookup(source, cached)) {
            UniqueFd cachedDescriptor(openReadFile(cached.path));
            OpenedFileInfo cachedInfo;
            if (cachedDescriptor.get() >= 0 &&
                inspectReadFile(cachedDescriptor.get(), cached.path, cachedInfo) &&
                cachedInfo.size == cached.size) {
                descriptor.swap(cachedDescriptor);
                responseLength = cachedInfo.size;
                compressed = true;
            }
        }
    }
    const auto etag = makeEtag(opened, compressed);

    const auto ifNoneMatch = header(connection, MHD_HTTP_HEADER_IF_NONE_MATCH, 2048);
    bool notModified = !ifNoneMatch.empty() && etagMatches(ifNoneMatch, etag);
    if (ifNoneMatch.empty()) {
        std::int64_t condition = 0;
        const auto ifModifiedSince =
            header(connection, MHD_HTTP_HEADER_IF_MODIFIED_SINCE, 128);
        notModified = parseHttpDate(ifModifiedSince, condition) &&
            opened.modifiedSeconds <= condition;
    }
    if (notModified) {
        static char empty = 0;
        auto* response = MHD_create_response_from_buffer(0, &empty, MHD_RESPMEM_PERSISTENT);
        if (response == nullptr) return MHD_NO;
        bool headersOk = addHeader(response, "Accept-Ranges", "bytes") &&
            addHeader(response, MHD_HTTP_HEADER_CACHE_CONTROL, "public, max-age=86400") &&
            addHeader(response, MHD_HTTP_HEADER_ETAG, etag) &&
            addHeader(response, MHD_HTTP_HEADER_LAST_MODIFIED, lastModified);
        if (config_.gzip) {
            headersOk = headersOk && addHeader(response, MHD_HTTP_HEADER_VARY,
                MHD_HTTP_HEADER_ACCEPT_ENCODING);
        }
        if (compressed) {
            headersOk = headersOk && addHeader(response,
                MHD_HTTP_HEADER_CONTENT_ENCODING, "gzip");
        }
        if (!headersOk) {
            MHD_destroy_response(response);
            return MHD_NO;
        }
        state.fileResponse = true;
        state.status = MHD_HTTP_NOT_MODIFIED;
        state.responseSize = 0;
        state.reason = "not modified";
        const int result = MHD_queue_response(connection, MHD_HTTP_NOT_MODIFIED, response);
        MHD_destroy_response(response);
        return result;
    }

    const std::uint64_t offset = partial ? range.start : 0;
    const std::uint64_t length = responseLength;
    auto context = std::make_unique<TransferContext>();
    context->fd = descriptor.release();
    context->sourceOffset = offset;
    context->length = length;
    context->state = &state;
    context->metrics = &metrics_;
    context->limiter = &bandwidth_;
    if (state.method == MHD_HTTP_METHOD_GET) context->lease = bandwidth_.attach(state.ip);

    auto* response = MHD_create_response_from_callback(
        length, kFileBlockSize, &fileReader, context.get(), &fileReaderFree);
    if (response == nullptr) return MHD_NO;
    context.release();

    const auto status = partial ? MHD_HTTP_PARTIAL_CONTENT : MHD_HTTP_OK;
    bool headersOk =
        addHeader(response, MHD_HTTP_HEADER_CONTENT_TYPE, contentType(file.extension)) &&
        addHeader(response, MHD_HTTP_HEADER_CACHE_CONTROL, "public, max-age=86400") &&
        addHeader(response, "Accept-Ranges", "bytes") &&
        addHeader(response, MHD_HTTP_HEADER_ETAG, etag) &&
        addHeader(response, MHD_HTTP_HEADER_LAST_MODIFIED, lastModified);
    if (headersOk && config_.gzip) {
        headersOk = addHeader(response, MHD_HTTP_HEADER_VARY,
            MHD_HTTP_HEADER_ACCEPT_ENCODING);
    }
    if (headersOk && compressed) {
        headersOk = addHeader(response, MHD_HTTP_HEADER_CONTENT_ENCODING, "gzip");
    }
    if (headersOk && partial) {
        const auto end = offset + length - 1;
        headersOk = addHeader(response, "Content-Range", "bytes " +
            std::to_string(offset) + "-" + std::to_string(end) + "/" +
            std::to_string(opened.size));
    }
    if (!headersOk) {
        MHD_destroy_response(response);
        return MHD_NO;
    }

    const int result = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    if (result == MHD_YES) {
        state.fileResponse = true;
        state.status = status;
        state.responseSize = length;
        state.reason.clear();
        if (state.method == MHD_HTTP_METHOD_GET) {
            state.activeTransfer = true;
            metrics_.transferStarted();
        }
    }
    return result;
}

std::string FastdlServer::clientIp(MHD_Connection* connection) {
    const auto* info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (info == nullptr || info->client_addr == nullptr) return "unknown";
    char buffer[INET_ADDRSTRLEN] = {};
    const auto* address = info->client_addr;
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) return buffer;
    }
    return "unknown";
}

std::string FastdlServer::header(
    MHD_Connection* connection, const char* name, std::size_t maxLength) {
    const char* value = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, name);
    std::string result;
    return boundedCopy(value, maxLength, result) ? result : std::string{};
}

bool FastdlServer::steamClient(const std::string& userAgent) {
    if (userAgent.rfind("Valve/Steam HTTP Client", 0) == 0) return true;
    if (userAgent.rfind("Steam/", 0) == 0) return true;
    return userAgent.find("Steam") != std::string::npos &&
        userAgent.find("HTTP") != std::string::npos &&
        userAgent.find("Client") != std::string::npos;
}

const char* FastdlServer::contentType(const std::string& extension) {
    if (extension == ".html" || extension == ".htm") return "text/html";
    if (extension == ".txt" || extension == ".res") return "text/plain";
    if (extension == ".wav") return "audio/wav";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".bmp") return "image/bmp";
    if (extension == ".tga") return "image/x-tga";
    if (extension == ".bz2") return "application/x-bzip2";
    if (extension == ".gz") return "application/gzip";
    return "application/octet-stream";
}

const char* FastdlServer::terminationName(MHD_RequestTerminationCode code) {
    switch (code) {
        case MHD_REQUEST_TERMINATED_COMPLETED_OK: return "ok";
        case MHD_REQUEST_TERMINATED_WITH_ERROR: return "error";
        case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED: return "timeout";
        case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN: return "shutdown";
        case MHD_REQUEST_TERMINATED_READ_ERROR: return "read error";
        case MHD_REQUEST_TERMINATED_CLIENT_ABORT: return "client abort";
        default: return "unknown";
    }
}

void FastdlServer::logCompleted(
    const RequestState& state, MHD_RequestTerminationCode code) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const auto bytes = state.fileResponse
        ? state.bytesSupplied.load(std::memory_order_relaxed) : state.responseSize;
    std::ostringstream line;
    line << '[' << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << "] "
         << sanitize(state.method, 16) << ' ' << sanitize(state.url, 2048)
         << " -> " << state.status
         << ' ' << kilobytes(bytes) << ' ' << terminationName(code)
         << " from " << sanitize(state.ip, 64) << " ["
         << sanitize(state.userAgent, 200) << ']';
    if (!state.reason.empty()) line << ' ' << state.reason;
    logger_.write(line.str());
}
