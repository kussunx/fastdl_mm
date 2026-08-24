#include "path_resolver.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;

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
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        "fastdl_mm_root_types_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    touch(root / "custom.wad", "wad");
    touch(root / "top.txt", "text");
    touch(root / "server.cfg", "private");
    touch(root / "maps/test.bsp", "bsp");

    std::string error;

    // Compatibility: the original overload keeps denying root-level files.
    PathResolver legacy;
    check(legacy.configure(root, "maps", "wad,bsp,txt", error),
        "configure legacy resolver");
    check(legacy.resolve("/custom.wad", 1024).status == ResolveStatus::DirectoryDenied,
        "legacy resolver still denies root-level wad");

    // ZPR enhancement by Kussun: explicitly allow WADs at fastdl_root.
    PathResolver resolver;
    check(resolver.configure(root, "maps", "wad,bsp,txt", "wad", error),
        "configure root wad allowlist");
    check(resolver.resolve("/custom.wad", 1024).status == ResolveStatus::Ok,
        "allow root-level wad");
    check(resolver.resolve("/top.txt", 1024).status == ResolveStatus::DirectoryDenied,
        "deny unrelated root-level allowed global type");
    check(resolver.resolve("/server.cfg", 1024).status == ResolveStatus::DirectoryDenied,
        "deny private root-level config");
    check(resolver.resolve("/maps/test.bsp", 1024).status == ResolveStatus::Ok,
        "subdirectory serving remains unchanged");

    // Root-level files must also pass the normal global extension allowlist.
    PathResolver globalNarrow;
    check(globalNarrow.configure(root, "maps", "bsp", "wad", error),
        "configure global type restriction");
    check(globalNarrow.resolve("/custom.wad", 1024).status == ResolveStatus::ExtensionDenied,
        "global type allowlist still gates root files");

    // Empty root types restores the original closed behavior explicitly.
    PathResolver closedRoot;
    check(closedRoot.configure(root, "maps", "wad,bsp,txt", "", error),
        "configure empty root allowlist");
    check(closedRoot.resolve("/custom.wad", 1024).status == ResolveStatus::DirectoryDenied,
        "empty root allowlist serves nothing at root");

    std::filesystem::remove_all(root, ec);
    if (failures != 0) {
        std::cerr << failures << " root type test(s) failed\n";
        return 1;
    }
    std::cout << "root type tests passed\n";
    return 0;
}
