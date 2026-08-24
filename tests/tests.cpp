#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "config_parser.h"
#include "fastdl_server.h"
#include "logger.h"
#include "path_resolver.h"
#include "rate_limiter.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;

// Local "YYYY-MM-DD" offset by whole days, matching the logger's naming.
std::string dayStamp(int offsetDays) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    local.tm_mday += offsetDays;
    local.tm_isdst = -1;
    std::mktime(&local);
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buffer.data();
}

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void touch(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}

void sizedFile(const std::filesystem::path& path, std::size_t bytes, char value = '\0') {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    std::array<char, 64 * 1024> block{};
    block.fill(value);
    while (bytes != 0) {
        const auto count = std::min(bytes, block.size());
        output.write(block.data(), static_cast<std::streamsize>(count));
        bytes -= count;
    }
}

#ifdef _WIN32
using RawSocket = SOCKET;
void closeRaw(RawSocket sock) { closesocket(sock); }
#else
using RawSocket = int;
constexpr RawSocket INVALID_SOCKET = -1;
void closeRaw(RawSocket sock) { close(sock); }
#endif

struct RawResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

RawResponse rawRequest(std::uint16_t port, const std::string& requestLine,
    const std::string& headers = {}, const std::string& userAgent =
        "Valve/Steam HTTP Client 1.0") {
    RawResponse result;
    const RawSocket sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return result;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeRaw(sock);
        return result;
    }

    const std::string request = requestLine +
        "\r\nHost: 127.0.0.1\r\nUser-Agent: " + userAgent + "\r\n" + headers +
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto count = ::send(sock, request.data() + sent,
            static_cast<int>(request.size() - sent), 0);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    while (response.size() < 32 * 1024 * 1024) {
        const auto received = ::recv(sock, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        response.append(buffer, static_cast<std::size_t>(received));
        const auto completeHeader = response.find("\r\n\r\n");
        if (completeHeader == std::string::npos) continue;
        const auto statusSpace = response.find(' ');
        const int status = statusSpace == std::string::npos
            ? 0 : std::atoi(response.c_str() + statusSpace + 1);
        if (requestLine.rfind("HEAD ", 0) == 0 || status == 204 || status == 304) break;
        const auto loweredHead = lower(response.substr(0, completeHeader));
        const auto lengthName = loweredHead.find("\r\ncontent-length:");
        if (lengthName != std::string::npos) {
            const auto valueStart = loweredHead.find(':', lengthName) + 1;
            const auto contentLength = static_cast<std::size_t>(
                std::strtoull(loweredHead.c_str() + valueStart, nullptr, 10));
            if (response.size() - completeHeader - 4 >= contentLength) break;
        }
    }
    closeRaw(sock);

    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return result;
    std::istringstream input(response.substr(0, headerEnd));
    std::string line;
    if (!std::getline(input, line) || line.rfind("HTTP/", 0) != 0) return result;
    const auto space = line.find(' ');
    if (space == std::string::npos) return result;
    result.status = std::atoi(line.c_str() + space + 1);
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? std::string() : value.substr(first);
        result.headers[lower(line.substr(0, colon))] = value;
    }
    result.body = response.substr(headerEnd + 4);
    return result;
}

int rawStatus(std::uint16_t port, const std::string& requestLine) {
    return rawRequest(port, requestLine).status;
}

void abortRequest(std::uint16_t port, const std::string& target) {
    const RawSocket sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        const std::string request = "GET " + target +
            " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "User-Agent: Valve/Steam HTTP Client 1.0\r\n\r\n";
        ::send(sock, request.data(), static_cast<int>(request.size()), 0);
#ifdef _WIN32
        shutdown(sock, SD_SEND);
#else
        shutdown(sock, SHUT_WR);
#endif
        char buffer[256];
        ::recv(sock, buffer, sizeof(buffer), 0);
    }
    closeRaw(sock);
}

bool gunzip(const std::string& compressed, std::string& output) {
    if (compressed.size() < 18 ||
        static_cast<unsigned char>(compressed[0]) != 0x1f ||
        static_cast<unsigned char>(compressed[1]) != 0x8b || compressed[2] != 8 ||
        compressed[3] != 0) return false;
    mz_stream stream{};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;
    stream.next_in = reinterpret_cast<const unsigned char*>(compressed.data() + 10);
    stream.avail_in = static_cast<unsigned int>(compressed.size() - 18);
    std::array<unsigned char, 64 * 1024> block{};
    int status = MZ_OK;
    output.clear();
    do {
        stream.next_out = block.data();
        stream.avail_out = static_cast<unsigned int>(block.size());
        status = mz_inflate(&stream, MZ_NO_FLUSH);
        if (status != MZ_OK && status != MZ_STREAM_END) break;
        output.append(reinterpret_cast<const char*>(block.data()),
            block.size() - stream.avail_out);
    } while (status != MZ_STREAM_END);
    mz_inflateEnd(&stream);
    if (status != MZ_STREAM_END) return false;

    const auto trailer = reinterpret_cast<const unsigned char*>(
        compressed.data() + compressed.size() - 8);
    const std::uint32_t expectedCrc = trailer[0] |
        (static_cast<std::uint32_t>(trailer[1]) << 8) |
        (static_cast<std::uint32_t>(trailer[2]) << 16) |
        (static_cast<std::uint32_t>(trailer[3]) << 24);
    const std::uint32_t expectedSize = trailer[4] |
        (static_cast<std::uint32_t>(trailer[5]) << 8) |
        (static_cast<std::uint32_t>(trailer[6]) << 16) |
        (static_cast<std::uint32_t>(trailer[7]) << 24);
    return expectedSize == static_cast<std::uint32_t>(output.size()) &&
        expectedCrc == static_cast<std::uint32_t>(mz_crc32(0,
            reinterpret_cast<const unsigned char*>(output.data()), output.size()));
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "fastdl_mm_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    touch(root / "maps/test.bsp", "bsp");
    touch(root / "maps/test.bsp.bz2", "compressed");
    touch(root / "maps/range.bsp", "0123456789abcdef");
    touch(root / "maps/secret.dll", "no");
    touch(root / "addons/config.txt", "private");
    touch(root / "liblist.gam", "top level");
    touch(root / "top.txt", "top level");
    touch(root / "custom.wad", "root wad");

    const std::string types = "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2";

    PathResolver resolver;
    std::string error;
    check(resolver.configure(root, "maps,models,sound", types, error),
        "configure test root");
    check(resolver.resolve("/maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "allow bsp");
    check(resolver.resolve("/maps/test.bsp.bz2", 1024).status == ResolveStatus::Ok,
        "allow bz2");
    check(resolver.resolve("/maps/secret.dll", 1024).status ==
        ResolveStatus::ExtensionDenied, "deny executable extension");
    check(resolver.resolve("/maps/test.bsp", 2).status == ResolveStatus::TooLarge,
        "enforce maximum file size");
    check(resolver.resolve("/../secret.bsp", 1024).status == ResolveStatus::InvalidPath,
        "deny dot-dot");
    // MHD unescapes before the handler, so "%2e%2e" is already ".." here.
    check(resolver.resolve("/maps/../secret.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny dot-dot after unescaping");
    // A literal '%' is an ordinary file name character, not an escape.
    check(resolver.resolve("/maps/100%.bsp", 1024).status ==
        ResolveStatus::NotFound, "percent is not decoded a second time");
    check(resolver.resolve("/maps\\test.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny backslash");
    check(resolver.resolve("/maps/test:bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny colon");
    check(resolver.resolve("/maps/test?x.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny question mark in a decoded path");
    check(resolver.resolve("/maps/test#x.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny fragment marker in a decoded path");
    check(resolver.resolve("/maps/test\x01.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "deny control characters");
    check(resolver.resolve("/maps", 1024).status == ResolveStatus::NotAFile,
        "directory is not a file");
    // Repeated separators collapse, so a trailing slash on sv_downloadurl costs
    // nothing: the client appends resource names verbatim.
    check(resolver.resolve("//maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "double slash from a trailing slash still resolves");
    check(resolver.resolve("/maps//test.bsp", 1024).status == ResolveStatus::Ok,
        "repeated separator anywhere is collapsed");
    check(resolver.resolve("///maps///test.bsp", 1024).status == ResolveStatus::Ok,
        "runs of separators are collapsed");
    check(resolver.resolve("/maps/", 1024).status == ResolveStatus::NotAFile,
        "trailing slash names the directory, not an invalid path");
    // Collapsing weakens nothing: a path is still only its components.
    check(resolver.resolve("/maps/..//test.bsp", 1024).status ==
        ResolveStatus::InvalidPath, "dot-dot beside a separator is still refused");
    check(resolver.resolve("//", 1024).status == ResolveStatus::InvalidPath,
        "separators alone name nothing");
    check(resolver.resolve("/", 1024).status == ResolveStatus::InvalidPath,
        "root alone names nothing");
    check(resolver.resolve("//addons/config.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "collapsing does not bypass the directory list");
    check(resolver.resolve("/maps/missing.wad", 1024).status ==
        ResolveStatus::NotFound, "missing file is identifiable");

    const auto outside = std::filesystem::temp_directory_path() /
        "fastdl_mm_tests_outside";
    std::filesystem::remove_all(outside, ec);
    touch(outside / "secret.bsp", "outside");
    std::filesystem::create_symlink(outside / "secret.bsp",
        root / "maps/escape.bsp", ec);
    if (!ec) {
        check(resolver.resolve("/maps/escape.bsp", 1024).status ==
            ResolveStatus::OutsideRoot, "deny a symlink escape from the content root");
    }
    ec.clear();

    // Directory allowlist.
    check(resolver.resolve("/addons/config.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny directory off the list");
    check(resolver.resolve("/liblist.gam", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny root-level file");
    check(resolver.resolve("/top.txt", 1024).status ==
        ResolveStatus::DirectoryDenied, "deny root-level file with allowed type");
#ifdef _WIN32
    check(resolver.resolve("/MAPS/test.bsp", 1024).status == ResolveStatus::Ok,
        "directory match is case insensitive");
#else
    // Case sensitive filesystem: the miss happens at canonical(), before the
    // allowlist is consulted.
    check(resolver.resolve("/MAPS/test.bsp", 1024).status == ResolveStatus::NotFound,
        "case-mismatched directory does not resolve");
#endif

    // Type allowlist is driven by the list, not a hardcoded table.
    PathResolver narrow;
    check(narrow.configure(root, "maps", "bsp", error), "configure narrow resolver");
    check(narrow.resolve("/maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "narrow list allows its one type");
    check(narrow.resolve("/maps/test.bsp.bz2", 1024).status ==
        ResolveStatus::ExtensionDenied, "narrow list denies everything else");

    // Dotted entries and "*" wildcards.
    PathResolver dotted;
    check(dotted.configure(root, "*", ".bsp, .txt", error), "configure dotted resolver");
    check(dotted.resolve("/addons/config.txt", 1024).status == ResolveStatus::Ok,
        "wildcard directory allows any subdirectory");
    check(dotted.resolve("/top.txt", 1024).status == ResolveStatus::DirectoryDenied,
        "wildcard still refuses root-level files");
    check(dotted.resolve("/maps/secret.dll", 1024).status ==
        ResolveStatus::ExtensionDenied, "dotted type entries parse");

    // Empty lists deny rather than allow.
    PathResolver closed;
    check(closed.configure(root, "", "", error), "configure empty resolver");
    check(closed.resolve("/maps/test.bsp", 1024).status == ResolveStatus::DirectoryDenied,
        "empty directory list serves nothing");

    // Config parser.
    {
        std::string name;
        std::string value;
        const auto parse = [&](const char* line) {
            name.clear();
            value.clear();
            return config::parseAssignment(line, name, value);
        };

        check(parse("fastdl_port \"27015\"") && name == "fastdl_port" && value == "27015",
            "quoted assignment");
        check(parse("fastdl_port 27015") && value == "27015", "bare assignment");
        check(parse("  fastdl_port   \"27015\"  ") && value == "27015",
            "surrounding whitespace");
        check(parse("fastdl_port \"27015\" // trailing note") && value == "27015",
            "trailing comment is removed");

        // A "//" inside quotes is data. Stripping comments before parsing quotes
        // truncated these.
        check(parse("fastdl_log \"logs//fastdl.log\"") && value == "logs//fastdl.log",
            "double slash inside a quoted value survives");
        check(parse("fastdl_x \"http://example.com/a\"") && value == "http://example.com/a",
            "url in a quoted value survives");
        check(parse("fastdl_x \"http://example.com/a\" // note") &&
            value == "http://example.com/a",
            "comment after a value containing slashes still strips");

        check(!parse("// fastdl_port \"27015\""), "comment-only line is not an assignment");
        check(!parse(""), "blank line");
        check(!parse("   \t  "), "whitespace-only line");
        check(!parse("fastdl_port"), "name with no value");
        check(!parse("fastdl_port \"27015"), "unterminated quote");
        check(!parse("fastdl_port \"27015\" junk"), "trailing junk after a quoted value");
        check(parse("fastdl_port \"27015\"\r") && value == "27015", "CRLF line ending");

        // Comment-only lines must strip to nothing, or loading a generated
        // config warns about every line in its comment block.
        check(config::trim(config::stripComment("// fastdl_log_age  keep (default \"7\")")).empty(),
            "generated comment line strips to nothing");
        check(config::stripComment("fastdl_log \"a//b\"") == "fastdl_log \"a//b\"",
            "stripComment leaves a quoted double slash alone");
    }

    RateLimiter limiter;
    check(!limiter.requestExceeded("127.0.0.1", 2), "first request allowed");
    check(!limiter.requestExceeded("127.0.0.1", 2), "second request allowed");
    check(limiter.requestExceeded("127.0.0.1", 2), "third request limited");
    // A rejected request must not be recorded, or a retrying client keeps its
    // own window full and never drains back under the limit.
    for (int i = 0; i < 50; ++i) limiter.requestExceeded("127.0.0.1", 2);
    check(!limiter.requestExceeded("127.0.0.1", 3), "rejections did not grow the window");
    check(limiter.requestExceeded("127.0.0.1", 3), "limit applies again once served");
    limiter.recordDenial("127.0.0.2", 2, 180);
    check(!limiter.blocked("127.0.0.2"), "first denial does not block");
    limiter.recordDenial("127.0.0.2", 2, 180);
    check(limiter.blocked("127.0.0.2"), "denial threshold blocks");
    check(limiter.unblock("127.0.0.2"), "explicit unblock");
    check(!limiter.blocked("127.0.0.2"), "unblocked client stays clear");

    {
        TransferMetrics metrics;
        metrics.reset();
        metrics.requestStarted();
        metrics.transferStarted();
        metrics.bytesSupplied(4096);
        metrics.transferFinished(TransferOutcome::Completed);
        metrics.transferStarted();
        metrics.transferFinished(TransferOutcome::Aborted);
        metrics.transferStarted();
        metrics.transferFinished(TransferOutcome::TimedOut);
        metrics.transferStarted();
        metrics.transferFinished(TransferOutcome::Failed);
        metrics.failure();
        const auto stats = metrics.snapshot();
        check(stats.requests == 1, "metrics count requests");
        check(stats.active == 0, "metrics return active transfers to zero");
        check(stats.completed == 1 && stats.aborted == 1 && stats.timedOut == 1 &&
            stats.failed == 2, "metrics classify transfer and pre-transfer failures");
        check(stats.transferredBytes == 4096, "metrics count supplied payload bytes");
        check(stats.throughputBitsPerSecond > 0.0, "metrics report recent throughput");
    }

    {
        BandwidthLimiter bandwidth;
        bandwidth.configure(0.0, 0.0);
        check(bandwidth.acquire({}, 65536) == 65536,
            "unlimited bandwidth grants a full block");
        bandwidth.stop();

        bandwidth.configure(0.0, 0.1);
        const auto first = bandwidth.attach("192.0.2.1");
        const auto second = bandwidth.attach("192.0.2.1");
        const auto started = std::chrono::steady_clock::now();
        std::size_t granted = 0;
        for (int i = 0; i < 20; ++i) {
            granted += bandwidth.acquire(i % 2 == 0 ? first : second, 65536);
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        check(granted == 20 * 1024, "small bandwidth rates use bounded quanta");
        check(elapsed >= 1.0 && elapsed < 4.0,
            "connections from one IP share the configured bandwidth");
        bandwidth.stop();

        bandwidth.configure(0.4, 0.0);
        std::atomic<std::size_t> globalGranted{0};
        const auto globalStarted = std::chrono::steady_clock::now();
        std::thread globalA([&] {
            for (int i = 0; i < 8; ++i) globalGranted.fetch_add(
                bandwidth.acquire({}, 65536), std::memory_order_relaxed);
        });
        std::thread globalB([&] {
            for (int i = 0; i < 8; ++i) globalGranted.fetch_add(
                bandwidth.acquire({}, 65536), std::memory_order_relaxed);
        });
        globalA.join();
        globalB.join();
        const auto globalElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - globalStarted).count();
        check(globalGranted.load(std::memory_order_relaxed) == 16 * 2500,
            "the global limiter bounds quanta across concurrent transfers");
        check(globalElapsed >= 0.45 && globalElapsed < 3.0,
            "the global bandwidth limit is shared across concurrent transfers");
        bandwidth.stop();

        bandwidth.configure(0.8, 0.4);
        const auto sharedA = bandwidth.attach("192.0.2.2");
        const auto sharedB = bandwidth.attach("192.0.2.2");
        std::atomic<std::size_t> combinedGranted{0};
        const auto combinedStarted = std::chrono::steady_clock::now();
        std::thread combinedA([&] {
            for (int i = 0; i < 8; ++i) combinedGranted.fetch_add(
                bandwidth.acquire(sharedA, 65536), std::memory_order_relaxed);
        });
        std::thread combinedB([&] {
            for (int i = 0; i < 8; ++i) combinedGranted.fetch_add(
                bandwidth.acquire(sharedB, 65536), std::memory_order_relaxed);
        });
        combinedA.join();
        combinedB.join();
        const auto combinedElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - combinedStarted).count();
        check(combinedGranted.load(std::memory_order_relaxed) == 16 * 2500,
            "combined global and per-IP limits grant bounded quanta");
        check(combinedElapsed >= 0.45 && combinedElapsed < 3.0,
            "the lower per-IP rate controls multiple connections when both limits apply");
        bandwidth.stop();

        bandwidth.configure(0.0, 0.4);
        const auto independentA = bandwidth.attach("192.0.2.3");
        const auto independentB = bandwidth.attach("192.0.2.4");
        const auto independentStarted = std::chrono::steady_clock::now();
        std::thread clientA([&] {
            for (int i = 0; i < 8; ++i) bandwidth.acquire(independentA, 65536);
        });
        std::thread clientB([&] {
            for (int i = 0; i < 8; ++i) bandwidth.acquire(independentB, 65536);
        });
        clientA.join();
        clientB.join();
        const auto independentElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - independentStarted).count();
        check(independentElapsed >= 0.15 && independentElapsed < 1.5,
            "different IP buckets refill independently");
        bandwidth.stop();

        bandwidth.configure(0.0, 0.1);
        const auto cancellable = bandwidth.attach("192.0.2.5");
        std::atomic<bool> cancelled{false};
        std::thread throttled([&] {
            while (bandwidth.acquire(cancellable, 65536) != 0) {
            }
            cancelled.store(true, std::memory_order_release);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        bandwidth.stop();
        throttled.join();
        check(cancelled.load(std::memory_order_acquire),
            "shutdown wakes a throttled transfer without a busy loop");
        bandwidth.configure(std::numeric_limits<double>::infinity(), -1.0);
        check(bandwidth.acquire({}, 65536) == 65536,
            "non-finite and negative limits safely fall back to unlimited mode");
        bandwidth.configure(0.0, 0.4);
        check(bandwidth.acquire(bandwidth.attach("192.0.2.6"), 512) == 512,
            "small transfers are not inflated to the limiter quantum");
        bandwidth.stop();
    }

    // Daily rotation and retention. Retention removes files, so check it keeps
    // what it should and leaves unrelated names alone.
    const auto logDir = std::filesystem::temp_directory_path() / "fastdl_mm_tests_log";
    std::filesystem::remove_all(logDir, ec);
    std::filesystem::create_directories(logDir, ec);
    const auto base = logDir / "fastdl.log";
    for (const int offset : {0, -2, -6, -7, -30}) {
        touch(logDir / ("fastdl-" + dayStamp(offset) + ".log"), "old\n");
    }
    touch(logDir / "fastdl.log", "not dated\n");
    touch(logDir / "other-2020-01-01.log", "unrelated\n");
    touch(logDir / "fastdl-notadate.log", "unrelated\n");

    {
        AsyncLogger logger;
        check(logger.start(base, 7), "logger starts");
        check(logger.pathForToday() == logDir / ("fastdl-" + dayStamp(0) + ".log"),
            "today's file is date stamped");
        logger.write("entry");
        logger.stop();
    }

    const auto exists = [&logDir](const std::string& name) {
        return std::filesystem::exists(logDir / name);
    };
    check(exists("fastdl-" + dayStamp(0) + ".log"), "keep today");
    check(exists("fastdl-" + dayStamp(-2) + ".log"), "keep within retention");
    check(exists("fastdl-" + dayStamp(-6) + ".log"), "keep oldest retained day");
    check(!exists("fastdl-" + dayStamp(-7) + ".log"), "drop the day past retention");
    check(!exists("fastdl-" + dayStamp(-30) + ".log"), "drop long expired");
    check(exists("fastdl.log"), "leave undated file alone");
    check(exists("other-2020-01-01.log"), "leave other prefixes alone");
    check(exists("fastdl-notadate.log"), "leave unparseable stamps alone");

    {
        std::ifstream today(logDir / ("fastdl-" + dayStamp(0) + ".log"));
        std::string contents((std::istreambuf_iterator<char>(today)),
            std::istreambuf_iterator<char>());
        check(contents.find("entry") != std::string::npos, "entry reaches today's file");
    }

    {
        AsyncLogger logger;
        check(logger.start(base, 0), "logger starts with retention disabled");
        logger.write("entry");
        logger.stop();
    }
    check(exists("fastdl-" + dayStamp(-30) + ".log") == false, "already removed stays removed");
    touch(logDir / ("fastdl-" + dayStamp(-30) + ".log"), "old\n");
    {
        AsyncLogger logger;
        check(logger.start(base, 0), "logger restarts with retention disabled");
        logger.write("entry");
        logger.stop();
    }
    check(exists("fastdl-" + dayStamp(-30) + ".log"),
        "retention 0 keeps everything");

    // A log file that cannot be opened reports the failure, and stays inert
    // afterwards: no thread is running, so writes must be discarded rather than
    // queued. The caller keeps serving with logging switched off.
    {
        const auto blockedDir =
            std::filesystem::temp_directory_path() / "fastdl_mm_tests_blocked";
        std::filesystem::remove_all(blockedDir, ec);
        std::filesystem::create_directories(blockedDir, ec);
        // Occupy today's name with a directory so the open cannot succeed.
        std::filesystem::create_directory(
            blockedDir / ("fastdl-" + dayStamp(0) + ".log"), ec);
        AsyncLogger logger;
        check(!logger.start(blockedDir / "fastdl.log", 7),
            "unwritable log path fails the start");
        for (int i = 0; i < 10000; ++i) logger.write("entry");
        logger.stop();
        check(std::filesystem::is_directory(
                  blockedDir / ("fastdl-" + dayStamp(0) + ".log"), ec),
            "writes after a failed start create no log file");
        std::filesystem::remove_all(blockedDir, ec);
    }

    // A directory that cannot be created is the same failure one level up: this
    // is what an HLDS root the server account cannot write looks like.
    {
        const auto blockedDir =
            std::filesystem::temp_directory_path() / "fastdl_mm_tests_blocked_parent";
        std::filesystem::remove_all(blockedDir, ec);
        std::filesystem::create_directories(blockedDir, ec);
        // A file where the log's parent directory needs to be.
        touch(blockedDir / "notadir", "occupied\n");
        AsyncLogger logger;
        check(!logger.start(blockedDir / "notadir" / "fastdl.log", 7),
            "uncreatable log directory fails the start");
        logger.write("entry");
        logger.stop();
        std::filesystem::remove_all(blockedDir, ec);
    }

    // Over a real socket, because the defect this guards against is in the
    // request line and never reaches a handler. The Steam client does not
    // percent-encode, so an asset with a space in its name arrives as
    // "GET /models/momoko/shu (2).mdl HTTP/1.1".
    {
#ifdef _WIN32
        WSADATA winsock{};
        WSAStartup(MAKEWORD(2, 2), &winsock);
#endif
        touch(root / "models/momoko/shu (2).mdl", "model");
        sizedFile(root / "maps/compress.bsp", 256 * 1024);

        FastdlConfig config;
        config.bindAddress = "127.0.0.1";
        config.port = 18973;
        config.root = root;
        config.baseDir = root;
        config.serveDirs = "maps,models";
        config.serveTypes = types;
        config.logPath = root / "logs" / "fastdl.log";
        config.logAgeDays = 1;
        config.serveRootTypes = "wad";

        FastdlServer server;
        std::string serverError;
        const bool started = server.start(config, serverError);
        check(started, "test server binds the loopback port");
        if (started) {
            check(rawStatus(config.port, "GET /maps/test.bsp HTTP/1.1") == 200,
                "an ordinary request line is unaffected");
            check(rawStatus(config.port,
                "GET /models/momoko/shu%20(2).mdl HTTP/1.1") == 200,
                "an encoded space is served");
            check(rawStatus(config.port,
                "GET /models/momoko/shu (2).mdl HTTP/1.1") == 200,
                "an unencoded space in the request target is served");
            // Splitting on the first space used to turn this request into the
            // one below it, which must stay a miss.
            check(rawStatus(config.port, "GET /models/momoko/shu HTTP/1.1") == 404,
                "the truncated name is still not a file");

            auto full = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1");
            check(full.status == 200 && full.body == "0123456789abcdef",
                "GET returns the complete representation");
            check(full.headers["content-length"] == "16",
                "GET returns the exact Content-Length");
            check(full.headers["accept-ranges"] == "bytes",
                "ordinary responses advertise byte ranges");
            check(!full.headers["last-modified"].empty() && !full.headers["etag"].empty(),
                "file responses include cache validators");

            auto head = rawRequest(config.port, "HEAD /maps/range.bsp HTTP/1.1");
            check(head.status == 200 && head.body.empty(), "HEAD returns no body");
            check(head.headers["content-length"] == "16",
                "HEAD preserves the GET Content-Length");
            auto headRange = rawRequest(config.port,
                "HEAD /maps/range.bsp HTTP/1.1", "Range: bytes=2-5\r\n");
            check(headRange.status == 206 && headRange.body.empty() &&
                headRange.headers["content-range"] == "bytes 2-5/16" &&
                headRange.headers["content-length"] == "4",
                "HEAD applies Range headers without returning a body");

            auto fixed = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1",
                "Range: bytes=2-5\r\n");
            check(fixed.status == 206 && fixed.body == "2345",
                "closed byte range returns the selected payload");
            check(fixed.headers["content-range"] == "bytes 2-5/16" &&
                fixed.headers["content-length"] == "4", "closed range headers are exact");

            auto openEnded = rawRequest(config.port,
                "GET /maps/range.bsp HTTP/1.1", "Range: bytes=10-\r\n");
            check(openEnded.status == 206 && openEnded.body == "abcdef",
                "open-ended byte range is supported");
            auto suffix = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1",
                "Range: bytes=-4\r\n");
            check(suffix.status == 206 && suffix.body == "cdef",
                "suffix byte range is supported");

            for (const auto& invalid : {"Range: bytes=99-\r\n",
                     "Range: bytes=0-1,4-5\r\n",
                     "Range: bytes=18446744073709551616-\r\n",
                     "Range: bytes=5-2\r\n"}) {
                auto response = rawRequest(
                    config.port, "GET /maps/range.bsp HTTP/1.1", invalid);
                check(response.status == 416 &&
                    response.headers["content-range"] == "bytes */16",
                    "invalid ranges return 416 with the complete size");
            }

            const auto byDate = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1",
                "If-Modified-Since: " + full.headers["last-modified"] + "\r\n");
            check(byDate.status == 304 && byDate.body.empty(),
                "If-Modified-Since returns 304");
            const auto byTag = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1",
                "If-None-Match: " + full.headers["etag"] + "\r\n");
            check(byTag.status == 304, "If-None-Match returns 304");
            const auto staleIfRange = rawRequest(config.port,
                "GET /maps/range.bsp HTTP/1.1",
                "Range: bytes=0-3\r\nIf-Range: \"stale\"\r\n");
            check(staleIfRange.status == 200 && staleIfRange.body == "0123456789abcdef",
                "a stale If-Range falls back to the complete representation");

            touch(root / "maps/range.bsp", "changed representation");
            const auto changed = rawRequest(config.port, "GET /maps/range.bsp HTTP/1.1",
                "If-None-Match: " + full.headers["etag"] + "\r\n");
            check(changed.status == 200 && changed.body == "changed representation",
                "source changes invalidate the ETag");

            check(rawRequest(config.port, "POST /maps/test.bsp HTTP/1.1").status == 403,
                "unsupported methods are denied");
            check(rawRequest(config.port, "GET /maps/test.bsp HTTP/1.1", {},
                "curl/8.0").status == 403, "non-Steam clients are denied by default");
            check(rawStatus(config.port, "GET /custom.wad HTTP/1.1") == 200,
                "root-level WADs are served over HTTP");
            check(rawStatus(config.port, "GET /top.txt HTTP/1.1") == 403,
                "other root-level files remain denied over HTTP");
            check(rawStatus(config.port, "GET /addons/config.txt HTTP/1.1") == 403,
                "unapproved directories remain denied over HTTP");
            check(rawStatus(config.port, "GET /maps/secret.dll HTTP/1.1") == 403,
                "unapproved extensions remain denied over HTTP");
            for (const auto& target : {"/%2e%2e/secret.bsp", "/maps/%2e%2e/secret.bsp",
                     "/maps%5ctest.bsp", "/maps/test%3absp", "/maps/test%23.bsp",
                     "/maps/test%01.bsp"}) {
                check(rawStatus(config.port, std::string("GET ") + target +
                    " HTTP/1.1") == 403, "encoded unsafe paths are denied");
            }

            const auto observed = server.stats();
            check(observed.active == 0, "completed HTTP requests leave no active transfer");
            check(observed.requests >= 20 && observed.completed >= 8,
                "server telemetry counts requests and completed file transfers");
            check(observed.transferredBytes < observed.completed * 1024,
                "telemetry counts delivered payload rather than full configured file sizes");

            sizedFile(root / "maps/abort.bsp", 8 * 1024 * 1024);
            abortRequest(config.port, "/maps/abort.bsp");
            for (int retry = 0; retry < 100 && server.stats().active != 0; ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto afterAbort = server.stats();
            check(afterAbort.active == 0, "client abort releases the active transfer");
            check(afterAbort.aborted + afterAbort.failed >= 1,
                "client abort is classified without claiming completion");
            server.stop();

            config.steamOnly = false;
            check(server.start(config, serverError),
                "server restarts with Steam filtering disabled");
            check(rawRequest(config.port, "GET /maps/test.bsp HTTP/1.1", {},
                "curl/8.0").status == 200,
                "ordinary clients work when Steam filtering is disabled");
            server.stop();

            const auto cachePath = root.parent_path() / "fastdl_mm_tests_gzip_cache";
            std::filesystem::remove_all(cachePath, ec);
            config.steamOnly = true;
            config.gzip = true;
            config.gzipCache = true;
            config.gzipCachePath = cachePath;
            config.gzipCacheMaxBytes = 16 * 1024 * 1024;
            check(server.start(config, serverError), "server starts with gzip cache enabled");
            auto cold = rawRequest(config.port, "GET /maps/compress.bsp HTTP/1.1",
                "Accept-Encoding: gzip,identity,*;q=0\r\n");
            check(cold.status == 200 && cold.headers["content-encoding"].empty() &&
                cold.body.size() == 256 * 1024,
                "a cache miss serves the original without blocking for compression");
            for (int retry = 0; retry < 500 &&
                 server.compressionStats().entries == 0; ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            auto warm = rawRequest(config.port, "GET /maps/compress.bsp HTTP/1.1",
                "Accept-Encoding: gzip,identity,*;q=0\r\n");
            std::string decompressed;
            check(warm.status == 200 && warm.headers["content-encoding"] == "gzip" &&
                warm.headers["vary"] == "Accept-Encoding",
                "a cache hit serves a negotiated gzip representation");
            check(warm.body.size() < 4096 && gunzip(warm.body, decompressed) &&
                decompressed == std::string(256 * 1024, '\0'),
                "cached gzip decompresses byte-for-byte to the source");
            auto compressedNotModified = rawRequest(config.port,
                "GET /maps/compress.bsp HTTP/1.1",
                "Accept-Encoding: gzip\r\nIf-None-Match: " + warm.headers["etag"] + "\r\n");
            check(compressedNotModified.status == 304 &&
                compressedNotModified.headers["content-encoding"] == "gzip" &&
                compressedNotModified.headers["vary"] == "Accept-Encoding",
                "compressed validators preserve representation metadata on 304");

            auto identity = rawRequest(config.port, "GET /maps/compress.bsp HTTP/1.1");
            check(identity.status == 200 && identity.headers["content-encoding"].empty() &&
                identity.body.size() == 256 * 1024,
                "clients that do not advertise gzip receive the source representation");
            auto rangedCompressed = rawRequest(config.port,
                "GET /maps/compress.bsp HTTP/1.1",
                "Accept-Encoding: gzip\r\nRange: bytes=0-1023\r\n");
            check(rangedCompressed.status == 206 &&
                rangedCompressed.headers["content-encoding"].empty() &&
                rangedCompressed.body.size() == 1024,
                "Range deterministically uses the uncompressed representation");

            sizedFile(root / "maps/compress.bsp", 256 * 1024, 'A');
            std::vector<RawResponse> concurrent(8);
            std::vector<std::thread> clients;
            for (std::size_t i = 0; i < concurrent.size(); ++i) {
                clients.emplace_back([&, i] {
                    concurrent[i] = rawRequest(config.port,
                        "GET /maps/compress.bsp HTTP/1.1", "Accept-Encoding: gzip\r\n");
                });
            }
            for (auto& client : clients) client.join();
            check(std::all_of(concurrent.begin(), concurrent.end(),
                [](const RawResponse& response) { return response.status == 200; }),
                "concurrent cache misses all fall back safely");
            for (int retry = 0; retry < 500 &&
                 server.compressionStats().entries < 2; ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            auto refreshed = rawRequest(config.port, "GET /maps/compress.bsp HTTP/1.1",
                "Accept-Encoding: gzip\r\n");
            check(gunzip(refreshed.body, decompressed) &&
                decompressed == std::string(256 * 1024, 'A'),
                "a source update creates a fresh compressed representation");
            check(server.compressionStats().entries == 2 &&
                server.compressionStats().failures == 0,
                "concurrent misses coordinate one cache artifact per source identity");

            server.buildCompressionCache();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            server.stop();
            check(!server.compressionStats().running,
                "shutdown cancels and joins cache work safely");
            std::filesystem::remove_all(cachePath, ec);

            config.gzipCachePath = root / "unsafe-cache";
            check(server.start(config, serverError),
                "an unsafe cache path does not prevent uncompressed service");
            check(!server.compressionError().empty(),
                "a cache path inside the public content root is rejected");
            auto safeFallback = rawRequest(config.port,
                "GET /maps/compress.bsp HTTP/1.1", "Accept-Encoding: gzip\r\n");
            check(safeFallback.status == 200 &&
                safeFallback.headers["content-encoding"].empty(),
                "cache configuration failure falls back to the source representation");
            server.stop();
        }
    }

    std::filesystem::remove_all(logDir, ec);
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(outside, ec);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
