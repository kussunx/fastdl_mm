#ifdef _WIN32
#include <winsock2.h>
#endif

#include <extdll.h>

// ReGameDLL's Metamod utility header includes util.h without cbase.h. These
// declarations are all util.h needs for a plugin that does not use game entities.
class CBaseEntity;
extern Vector g_vecZero;

#include <meta_api.h>

// Valve's HLSDK pulls minmax.h in through extdll.h, which defines min, max and
// clamp as macros and breaks the std:: versions at their call sites. ReGameDLL's
// headers keep them in mathlib.h, which extdll.h does not include, so this is a
// no-op there.
#undef min
#undef max
#undef clamp

#include "config.h"
#include "config_parser.h"
#include "fastdl_server.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#ifndef FASTDL_MM_VERSION
#define FASTDL_MM_VERSION "dev"
#endif

enginefuncs_t g_engfuncs;
globalvars_t* gpGlobals = nullptr;
meta_globals_t* gpMetaGlobals = nullptr;
gamedll_funcs_t* gpGamedllFuncs = nullptr;
mutil_funcs_t* gpMetaUtilFuncs = nullptr;

namespace {
FastdlServer g_server;
bool g_registered = false;
bool g_startPending = false;
std::filesystem::path g_baseDir;
bool g_downloadUrlManaged = false;
std::string g_previousDownloadUrl;

cvar_t cvarEnabled = {"fastdl_enabled", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBind = {"fastdl_bind", "0.0.0.0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarPort = {"fastdl_port", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarRoot = {"fastdl_root", "cstrike", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarServeDirs = {"fastdl_serve_dirs",
    "sprites,sound,sounds,overviews,models,maps,gfx", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarServeTypes = {"fastdl_serve_types",
    "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2",
    FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarServeRootTypes = {"fastdl_serve_root_types", "wad", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarMaxFileMb = {"fastdl_max_file_mb", "250", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarThreads = {"fastdl_threads", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarMaxConnections = {"fastdl_max_connections", "64", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarConnections = {"fastdl_max_connections_ip", "32", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarConnectionTimeout = {
    "fastdl_connection_timeout", "30", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarRequests = {"fastdl_requests_minute", "500", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarDenials = {"fastdl_denials_minute", "36", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBlockSeconds = {"fastdl_block_seconds", "180", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBandwidthMax = {"fastdl_bandwidth_max_mbps", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarBandwidthIp = {"fastdl_bandwidth_ip_mbps", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarSteamOnly = {"fastdl_steam_only", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarGzip = {"fastdl_gzip", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarGzipCache = {"fastdl_gzip_cache", "1", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarGzipCachePath = {
    "fastdl_gzip_cache_path", "fastdl_cache", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarGzipCacheMaxMb = {
    "fastdl_gzip_cache_max_mb", "256", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarAutoDownloadUrl = {"fastdl_auto_downloadurl", "0", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarPublicUrl = {"fastdl_public_url", "", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarLog = {"fastdl_log", "logs/fastdl/fastdl.log", FCVAR_SERVER, 0.0f, nullptr};
cvar_t cvarLogAge = {"fastdl_log_age", "7", FCVAR_SERVER, 0.0f, nullptr};

// Registration order, and the order a generated config is written in. The help
// text is only used for the comment block in a generated config.
struct Setting {
    cvar_t* cvar;
    const char* help;
};

const Setting g_settings[] = {
    {&cvarEnabled,        "toggle fastdl hosting"},
    {&cvarBind,           "listen address"},
    {&cvarPort,           "listen port (TCP); 0 uses the game port number"},
    {&cvarRoot,           "game mod directory"},
    {&cvarServeDirs,      "subdirectories served, comma separated"},
    {&cvarServeTypes,     "extensions served, comma separated"},
    {&cvarServeRootTypes, "root-level extensions served, comma separated"},
    {&cvarMaxFileMb,      "refuse files larger than this"},
    {&cvarThreads,        "HTTP workers; use 2 only for measured concurrent/slow transfers"},
    {&cvarMaxConnections, "global concurrent connection limit"},
    {&cvarConnections,    "concurrent connections per IP"},
    {&cvarConnectionTimeout, "idle connection timeout in seconds"},
    {&cvarRequests,       "requests per minute per IP"},
    {&cvarDenials,        "denials per minute before a temporary block"},
    {&cvarBlockSeconds,   "how long a block lasts, in seconds"},
    {&cvarBandwidthMax,   "global response payload limit in Mbps; 0 is unlimited"},
    {&cvarBandwidthIp,    "shared response payload limit per IP; 0 is unlimited"},
    {&cvarSteamOnly,      "only accept the Steam downloader User-Agent"},
    {&cvarGzip,           "serve cached gzip when the client accepts it"},
    {&cvarGzipCache,      "prepare gzip representations on a background thread"},
    {&cvarGzipCachePath,  "disk cache path outside fastdl_root"},
    {&cvarGzipCacheMaxMb, "maximum compressed cache size"},
    {&cvarAutoDownloadUrl,"set sv_downloadurl after a successful start"},
    {&cvarPublicUrl,      "explicit public HTTP URL used by automatic mode"},
    {&cvarLog,            "log base path; one file per day"},
    {&cvarLogAge,         "days of logs to keep, 0 keeps all"}
};

unsigned int boundedUnsigned(const char* name, unsigned int minimum,
    unsigned int maximum, unsigned int fallback) {
    const float value = g_engfuncs.pfnCVarGetFloat(name);
    if (!std::isfinite(value)) return fallback;
    return static_cast<unsigned int>(std::clamp(value,
        static_cast<float>(minimum), static_cast<float>(maximum)));
}

double boundedRate(const char* name) {
    const float value = g_engfuncs.pfnCVarGetFloat(name);
    if (!std::isfinite(value) || value <= 0.0f) return 0.0;
    return static_cast<double>(std::clamp(value, 0.1f, 10000.0f));
}

// The port HLDS serves the game on. Managed hosts allocate and firewall ports
// by number regardless of protocol, so TCP on the UDP game port number is the
// likeliest to be reachable without a second allocation. ReHLDS sets hostport
// from -port at startup (net_ws.cpp); "port" is the fallback.
unsigned int gamePort() {
    for (const char* name : {"hostport", "port"}) {
        const float value = g_engfuncs.pfnCVarGetFloat(name);
        if (value >= 1.0f && value <= 65535.0f) return static_cast<unsigned int>(value);
    }
    return 27015;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Relative roots and log paths resolve against the HLDS directory, not the
// working directory a service wrapper is free to change. The plugin sits at
// <base>/<mod>/addons/..., so walk up to the mod directory.
std::filesystem::path resolveBaseDir() {
    std::error_code ec;
    char gameDir[512] = {};
    g_engfuncs.pfnGetGameDir(gameDir);
    const auto modName =
        lowerAscii(std::filesystem::u8path(gameDir).filename().u8string());

    const char* pluginPath =
        gpMetaUtilFuncs != nullptr ? gpMetaUtilFuncs->pfnGetPluginPath(PLID) : nullptr;
    if (pluginPath != nullptr && !modName.empty()) {
        const auto absolute =
            std::filesystem::absolute(std::filesystem::u8path(pluginPath), ec);
        if (!ec) {
            for (auto dir = absolute.parent_path();; dir = dir.parent_path()) {
                if (lowerAscii(dir.filename().u8string()) == modName) {
                    // canonical() also settles drive letter case.
                    auto base = std::filesystem::canonical(dir.parent_path(), ec);
                    return ec ? dir.parent_path() : base;
                }
                if (dir == dir.parent_path()) break;
            }
        }
    }
    const auto working = std::filesystem::current_path(ec);
    if (ec) return {};
    auto canonical = std::filesystem::canonical(working, ec);
    return ec ? working : canonical;
}

FastdlConfig readConfig() {
    FastdlConfig config;
    config.enabled = g_engfuncs.pfnCVarGetFloat("fastdl_enabled") != 0.0f;
    config.bindAddress = g_engfuncs.pfnCVarGetString("fastdl_bind");
    const unsigned int port = boundedUnsigned("fastdl_port", 0, 65535, 0);
    config.port = static_cast<std::uint16_t>(port == 0 ? gamePort() : port);
    config.root = std::filesystem::u8path(g_engfuncs.pfnCVarGetString("fastdl_root"));
    config.baseDir = g_baseDir;
    config.serveDirs = g_engfuncs.pfnCVarGetString("fastdl_serve_dirs");
    config.serveTypes = g_engfuncs.pfnCVarGetString("fastdl_serve_types");
    config.serveRootTypes = g_engfuncs.pfnCVarGetString("fastdl_serve_root_types");
    config.maxFileBytes =
        static_cast<std::uint64_t>(boundedUnsigned("fastdl_max_file_mb", 1, 2048, 250)) *
        1024ULL * 1024ULL;
    // Workers exist to overlap blocking file reads, not to add CPU: connections
    // are pinned per worker, so one uncached read stalls only its own share.
    // Cached reads never block, so more than a few is pure scheduler contention
    // against the single game thread.
    config.threads = boundedUnsigned("fastdl_threads", 1, 4, 1);
    config.maxConnections = boundedUnsigned("fastdl_max_connections", 8, 128, 64);
    config.maxConnectionsPerIp =
        boundedUnsigned("fastdl_max_connections_ip", 1, config.maxConnections, 32);
    config.connectionTimeoutSeconds =
        boundedUnsigned("fastdl_connection_timeout", 5, 300, 30);
    config.requestsPerMinute = boundedUnsigned("fastdl_requests_minute", 1, 10000, 500);
    config.denialsPerMinute = boundedUnsigned("fastdl_denials_minute", 1, 1000, 36);
    config.blockSeconds = boundedUnsigned("fastdl_block_seconds", 1, 86400, 180);
    config.bandwidthMaxMbps = boundedRate("fastdl_bandwidth_max_mbps");
    config.bandwidthIpMbps = boundedRate("fastdl_bandwidth_ip_mbps");
    config.steamOnly = g_engfuncs.pfnCVarGetFloat("fastdl_steam_only") != 0.0f;
    config.gzip = g_engfuncs.pfnCVarGetFloat("fastdl_gzip") != 0.0f;
    config.gzipCache = g_engfuncs.pfnCVarGetFloat("fastdl_gzip_cache") != 0.0f;
    config.gzipCachePath =
        std::filesystem::u8path(g_engfuncs.pfnCVarGetString("fastdl_gzip_cache_path"));
    config.gzipCacheMaxBytes = static_cast<std::uint64_t>(
        boundedUnsigned("fastdl_gzip_cache_max_mb", 16, 1024, 256)) * 1024ULL * 1024ULL;
    config.autoDownloadUrl =
        g_engfuncs.pfnCVarGetFloat("fastdl_auto_downloadurl") != 0.0f;
    config.publicUrl = g_engfuncs.pfnCVarGetString("fastdl_public_url");
    config.logPath = std::filesystem::u8path(g_engfuncs.pfnCVarGetString("fastdl_log"));
    config.logAgeDays = boundedUnsigned("fastdl_log_age", 0, 3650, 7);
    return config;
}

void print(const std::string& text) {
    g_engfuncs.pfnServerPrint(("[FastDL] " + text + "\n").c_str());
}

std::string humanBytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
              << value << ' ' << units[unit];
    return formatted.str();
}

std::string humanRate(double bitsPerSecond) {
    std::ostringstream formatted;
    if (bitsPerSecond >= 1000000.0) {
        formatted << std::fixed << std::setprecision(1)
                  << bitsPerSecond / 1000000.0 << " Mbps";
    } else if (bitsPerSecond >= 1000.0) {
        formatted << std::fixed << std::setprecision(1)
                  << bitsPerSecond / 1000.0 << " Kbps";
    } else {
        formatted << std::fixed << std::setprecision(0) << bitsPerSecond << " bps";
    }
    return formatted.str();
}

std::string configuredRate(double mbps) {
    if (mbps <= 0.0) return "unlimited";
    return humanRate(mbps * 1000000.0);
}

void restoreDownloadUrl() {
    if (!g_downloadUrlManaged) return;
    g_engfuncs.pfnCVarSetString("sv_downloadurl", g_previousDownloadUrl.c_str());
    g_previousDownloadUrl.clear();
    g_downloadUrlManaged = false;
}

bool normalizedPublicUrl(const std::string& configured, std::string& normalized) {
    normalized = config::trim(configured);
    if (normalized.rfind("http://", 0) != 0 || normalized.size() < 8) return false;
    for (const unsigned char c : normalized) {
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '#') return false;
    }
    const auto authorityEnd = normalized.find('/', 7);
    const auto authority = normalized.substr(7,
        authorityEnd == std::string::npos ? std::string::npos : authorityEnd - 7);
    if (authority.empty() || authority.front() == ':' || authority.back() == ':') return false;
    if (normalized.back() != '/') normalized.push_back('/');
    return normalized.size() <= 127;
}

void configureDownloadUrl(const FastdlConfig& config) {
    if (!config.autoDownloadUrl) return;
    std::string url;
    if (!normalizedPublicUrl(config.publicUrl, url)) {
        print("automatic sv_downloadurl skipped: fastdl_public_url must be a valid "
              "http:// URL of at most 127 characters");
        return;
    }
    g_previousDownloadUrl = g_engfuncs.pfnCVarGetString("sv_downloadurl");
    g_engfuncs.pfnCVarSetString("sv_downloadurl", url.c_str());
    g_downloadUrlManaged = true;
    print("sv_downloadurl set to " + url);
}

// An sv_downloadurl nothing is serving costs each client a connect timeout per
// file before it falls back, which looks like a broken download. Whether the
// URL refers to this machine cannot be checked -- the engine only knows its
// local address -- so warn rather than edit the cvar on a guess.
void warnIfDownloadUrlSet() {
    const std::string url = g_engfuncs.pfnCVarGetString("sv_downloadurl");
    if (url.empty()) return;
    print("not serving, but sv_downloadurl is set (" + url +
        ") - clients will stall on it before falling back");
}

void printLogState() {
    if (!g_server.logging()) {
        print("NOT logging: could not open " + g_server.logPath().u8string() +
            " (check the directory exists and the server account can write it)");
        return;
    }
    print("log " + g_server.logPath().u8string() + ", keeping " +
        (g_server.config().logAgeDays == 0
            ? std::string("all days")
            : std::to_string(g_server.config().logAgeDays) + " days"));
}

void startServer() {
    restoreDownloadUrl();
    const auto config = readConfig();
    if (!config.enabled) {
        g_server.stop();
        print("disabled");
        warnIfDownloadUrlSet();
        return;
    }
    std::string error;
    if (!g_server.start(config, error)) {
        print("start failed: " + error);
        warnIfDownloadUrlSet();
        return;
    }
    const bool followsGamePort = g_engfuncs.pfnCVarGetFloat("fastdl_port") == 0.0f;
    print("listening on " + config.bindAddress + ":" + std::to_string(config.port) +
        (followsGamePort ? " (game port)" : "") +
        ", threads=" + std::to_string(config.threads) +
        ", root=" + g_server.root().u8string());
    print("serving " + config.serveDirs + "; root types=" + config.serveRootTypes);
    if (!g_server.compressionError().empty()) {
        print("gzip disabled: " + g_server.compressionError());
    }
    configureDownloadUrl(config);
    printLogState();
}

void commandRestart() {
    try {
        print("restarting");
        startServer();
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] restart failed with an exception\n");
    }
}

void commandStatus() {
    try {
        if (!g_server.running()) {
            print("stopped");
            return;
        }
        const auto& config = g_server.config();
        print("running on " + config.bindAddress + ":" + std::to_string(config.port) +
            ", root=" + g_server.root().u8string());
        print("dirs=" + config.serveDirs);
        print("types=" + config.serveTypes);
        print("root-types=" + config.serveRootTypes);
        print("threads=" + std::to_string(config.threads) +
            ", connections=" + std::to_string(config.maxConnections) +
            ", conn/ip=" + std::to_string(config.maxConnectionsPerIp) +
            ", timeout=" + std::to_string(config.connectionTimeoutSeconds) + "s" +
            ", req/min=" + std::to_string(config.requestsPerMinute) +
            ", denials/min=" + std::to_string(config.denialsPerMinute) +
            ", block=" + std::to_string(config.blockSeconds) + "s");
        print("bandwidth global=" + configuredRate(config.bandwidthMaxMbps) +
            ", per-IP=" + configuredRate(config.bandwidthIpMbps) +
            ", Steam-only=" + (config.steamOnly ? "on" : "off") +
            ", auto-url=" + (config.autoDownloadUrl ? "on" : "off"));
        const auto compression = g_server.compressionStats();
        print("gzip=" + std::string(config.gzip ? "on" : "off") +
            ", cache=" + (compression.running ? "running" :
                (config.gzipCache ? "stopped" : "off")) +
            ", cache-path=" + config.gzipCachePath.generic_u8string());
        if (!g_server.compressionError().empty()) {
            print("gzip cache error: " + g_server.compressionError());
        }
        printLogState();
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] status failed with an exception\n");
    }
}

void commandStats() {
    try {
        const auto stats = g_server.stats();
        print("requests=" + std::to_string(stats.requests) +
            ", active=" + std::to_string(stats.active) +
            ", completed=" + std::to_string(stats.completed));
        print("aborted=" + std::to_string(stats.aborted) +
            ", timeouts=" + std::to_string(stats.timedOut) +
            ", errors=" + std::to_string(stats.failed));
        print("transferred=" + humanBytes(stats.transferredBytes) +
            ", recent throughput=" + humanRate(stats.throughputBitsPerSecond));
        const auto compression = g_server.compressionStats();
        print("gzip cache entries=" + std::to_string(compression.entries) +
            " (" + humanBytes(compression.bytes) + "), queued=" +
            std::to_string(compression.queued) + ", hits=" +
            std::to_string(compression.hits) + ", misses=" +
            std::to_string(compression.misses) + ", errors=" +
            std::to_string(compression.failures));
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] stats failed with an exception\n");
    }
}

void commandCacheBuild() {
    try {
        if (!g_server.compressionStats().running) {
            print("gzip cache is not running; enable fastdl_gzip and fastdl_gzip_cache");
            return;
        }
        g_server.buildCompressionCache();
        print("gzip cache build queued on the background worker");
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] cache build failed with an exception\n");
    }
}

void commandUnblock() {
    try {
        if (g_engfuncs.pfnCmd_Argc() != 2) {
            print("usage: fastdl_unblock <ip>");
            return;
        }
        const std::string ip = g_engfuncs.pfnCmd_Argv(1);
        print(g_server.unblock(ip) ? "unblocked " + ip : "no state for " + ip);
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] unblock failed with an exception\n");
    }
}

// Values are read back from the cvars, so a generated file always matches what
// the plugin actually registered. Call only after registration.
bool writeDefaultConfig(const std::filesystem::path& path, std::string& error) {
    std::size_t column = 0;
    for (const auto& setting : g_settings) {
        column = std::max(column, std::strlen(setting.cvar->name));
    }
    column += 4;
    const int width = static_cast<int>(column);

    std::ofstream out(path);
    if (!out) {
        error = "could not create " + path.generic_u8string();
        return false;
    }
    for (const auto& setting : g_settings) {
        out << std::left << std::setw(width) << setting.cvar->name
            << '"' << setting.cvar->string << "\"\n";
    }
    out << "\n// remember to set sv_downloadurl to \"http://your-ip:port/\"\n"
           "\n// CVARs:\n";
    for (const auto& setting : g_settings) {
        out << "// " << std::left << std::setw(width) << setting.cvar->name
            << setting.help << " (default \"" << setting.cvar->string << "\")\n";
    }
    out.flush();
    if (!out) {
        error = "could not write " + path.generic_u8string();
        return false;
    }
    return true;
}

// Bad lines are reported and skipped, not fatal: failing the whole file would
// silently revert every setting -- including the port -- to its compiled-in
// default whenever the config is newer than the binary beside it.
bool loadConfigFile(const std::filesystem::path& path, unsigned int& loaded,
    unsigned int& skipped, std::string& error) {
    std::ifstream configFile(path);
    if (!configFile) {
        error = "could not open " + path.generic_u8string();
        return false;
    }

    loaded = 0;
    skipped = 0;
    std::string line;
    unsigned int lineNumber = 0;
    while (std::getline(configFile, line)) {
        ++lineNumber;
        std::string name;
        std::string value;
        if (!config::parseAssignment(line, name, value)) {
            if (!config::trim(config::stripComment(line)).empty()) {
                print("config: skipping invalid assignment on line " +
                    std::to_string(lineNumber));
                ++skipped;
            }
            continue;
        }
        const bool known = std::any_of(std::begin(g_settings), std::end(g_settings),
            [&name](const Setting& setting) { return name == setting.cvar->name; });
        if (!known) {
            print("config: skipping unknown setting '" + name + "' on line " +
                std::to_string(lineNumber));
            ++skipped;
            continue;
        }
        g_engfuncs.pfnCVarSetString(name.c_str(), value.c_str());
        ++loaded;
    }
    if (configFile.bad()) {
        error = "failed while reading " + path.generic_u8string();
        return false;
    }
    return true;
}

void registerPlugin() {
    if (g_registered) return;
    for (const auto& setting : g_settings) g_engfuncs.pfnCVarRegister(setting.cvar);
    g_engfuncs.pfnAddServerCommand("fastdl_restart", &commandRestart);
    g_engfuncs.pfnAddServerCommand("fastdl_status", &commandStatus);
    g_engfuncs.pfnAddServerCommand("fastdl_stats", &commandStats);
    g_engfuncs.pfnAddServerCommand("fastdl_cache_build", &commandCacheBuild);
    g_engfuncs.pfnAddServerCommand("fastdl_unblock", &commandUnblock);
    g_baseDir = resolveBaseDir();
    print("base directory " + g_baseDir.u8string());
    const char* pluginPath =
        gpMetaUtilFuncs != nullptr ? gpMetaUtilFuncs->pfnGetPluginPath(PLID) : nullptr;
    const std::filesystem::path configPath =
        std::filesystem::u8path(pluginPath ? pluginPath : "addons/fastdl/fastdl_mm.dll")
            .parent_path() / "fastdl_mm.cfg";
    print("loading config " + configPath.generic_u8string());
    // Only when genuinely absent: a file we cannot stat is never overwritten.
    std::error_code existsError;
    if (!std::filesystem::exists(configPath, existsError) && !existsError) {
        std::string writeError;
        if (writeDefaultConfig(configPath, writeError)) {
            print("no config found, wrote defaults");
        } else {
            print("no config found and " + writeError);
        }
    }
    unsigned int loaded = 0;
    unsigned int skipped = 0;
    std::string configError;
    if (!loadConfigFile(configPath, loaded, skipped, configError)) {
        print("config error: " + configError);
    } else {
        print("config applied: " + std::to_string(loaded) + " settings" +
            (skipped != 0 ? ", " + std::to_string(skipped) + " skipped" : "") +
            ", port=" + std::string(g_engfuncs.pfnCVarGetString("fastdl_port")) +
            ", root=" + g_engfuncs.pfnCVarGetString("fastdl_root"));
    }
    g_registered = true;
    g_startPending = true;
}

void gameInit() {
    try {
        registerPlugin();
    } catch (...) {
        g_engfuncs.pfnServerPrint("[FastDL] initialization failed with an exception\n");
    }
    gpMetaGlobals->mres = MRES_IGNORED;
}

void startFrame() {
    try {
        if (g_startPending) {
            g_startPending = false;
            startServer();
        }
    } catch (...) {
        g_startPending = false;
        g_engfuncs.pfnServerPrint("[FastDL] deferred start failed with an exception\n");
    }
    gpMetaGlobals->mres = MRES_IGNORED;
}

DLL_FUNCTIONS functions = {};

int getEntityApi2(DLL_FUNCTIONS* table, int* version) {
    if (table == nullptr || version == nullptr) return FALSE;
    if (*version != INTERFACE_VERSION) {
        *version = INTERFACE_VERSION;
        return FALSE;
    }
    functions.pfnGameInit = &gameInit;
    functions.pfnStartFrame = &startFrame;
    std::memcpy(table, &functions, sizeof(functions));
    return TRUE;
}
} // namespace

plugin_info_t Plugin_info = {
    META_INTERFACE_VERSION,
    "FastDL",
    FASTDL_MM_VERSION,
    __DATE__,
    "servor",
    "",
    "FASTDL",
    PT_STARTUP,
    // Never unloadable. Cvar_RegisterVariable links our cvar_t structs into the
    // engine's list by pointer and there is no way to take them back out, and
    // AddServerCommand keeps handlers pointing into this DLL. Unmapping it --
    // by "meta unload" or by Metamod's own reload when the file on disk is
    // newer -- would leave the engine reading and calling freed memory.
    PT_NEVER
};

C_DLLEXPORT void WINAPI GiveFnptrsToDll(
    enginefuncs_t* engineFunctions, globalvars_t* globals) {
    std::memcpy(&g_engfuncs, engineFunctions, sizeof(g_engfuncs));
    gpGlobals = globals;
}

C_DLLEXPORT int Meta_Query(
    char*, plugin_info_t** pluginInfo, mutil_funcs_t* utilityFunctions) {
    if (pluginInfo == nullptr || utilityFunctions == nullptr) return FALSE;
    *pluginInfo = &Plugin_info;
    gpMetaUtilFuncs = utilityFunctions;
    return TRUE;
}

META_FUNCTIONS metaFunctions = {
    nullptr,
    nullptr,
    &getEntityApi2,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME, META_FUNCTIONS* functionTable,
    meta_globals_t* globals, gamedll_funcs_t* gameFunctions) {
    if (functionTable == nullptr || globals == nullptr) return FALSE;
    gpMetaGlobals = globals;
    gpGamedllFuncs = gameFunctions;
    std::memcpy(functionTable, &metaFunctions, sizeof(metaFunctions));
    return TRUE;
}

C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME, PL_UNLOAD_REASON) {
    try {
        restoreDownloadUrl();
        g_server.stop();
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}
