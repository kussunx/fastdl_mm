#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct FastdlConfig {
    bool enabled = true;
    std::string bindAddress = "0.0.0.0";
    // 0 selects the game port number, on TCP.
    std::uint16_t port = 0;
    std::filesystem::path root;
    // Relative roots and log paths resolve against this, not the working
    // directory. Set from the plugin location at load time.
    std::filesystem::path baseDir;
    // Comma separated. "*" allows everything, empty allows nothing.
    std::string serveDirs = "sprites,sound,sounds,overviews,models,maps,gfx";
    std::string serveTypes = "bsp,nav,res,wad,mdl,spr,wav,mp3,bmp,tga,txt,htm,html,gz,bz2";
    // Root-level files are denied unless their extension is explicitly listed
    // here. Keep this narrow: GoldSrc WADs are the intended use case.
    std::string serveRootTypes = "wad";
    std::uint64_t maxFileBytes = 250ULL * 1024ULL * 1024ULL;
    unsigned int threads = 1;
    unsigned int maxConnections = 64;
    unsigned int maxConnectionsPerIp = 32;
    unsigned int connectionTimeoutSeconds = 30;
    unsigned int requestsPerMinute = 500;
    unsigned int denialsPerMinute = 36;
    unsigned int blockSeconds = 180;
    double bandwidthMaxMbps = 0.0;
    double bandwidthIpMbps = 0.0;
    bool steamOnly = true;
    bool gzip = false;
    bool gzipCache = true;
    std::filesystem::path gzipCachePath = "fastdl_cache";
    std::uint64_t gzipCacheMaxBytes = 256ULL * 1024ULL * 1024ULL;
    bool autoDownloadUrl = false;
    std::string publicUrl;
    std::filesystem::path logPath = "logs/fastdl/fastdl.log";
    unsigned int logAgeDays = 7;
};
