#include "secure_file.h"

#include <limits>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int openReadFile(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wopen(path.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    return open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

void closeReadFile(int descriptor) {
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
}

#ifdef _WIN32
namespace {
std::filesystem::path pathFromHandle(HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) return {};
    std::vector<wchar_t> value(static_cast<std::size_t>(required) + 1);
    const DWORD written = GetFinalPathNameByHandleW(
        handle, value.data(), static_cast<DWORD>(value.size()), flags);
    if (written == 0 || written >= value.size()) return {};
    std::wstring path(value.data(), written);
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        path = L"\\\\" + path.substr(8);
    } else if (path.rfind(L"\\\\?\\", 0) == 0) {
        path.erase(0, 4);
    }
    return std::filesystem::path(path);
}
} // namespace
#endif

bool inspectReadFile(int descriptor, const std::filesystem::path& expected,
    OpenedFileInfo& info) {
#ifdef _WIN32
    const auto rawHandle = _get_osfhandle(descriptor);
    if (rawHandle == -1) return false;
    const auto handle = reinterpret_cast<HANDLE>(rawHandle);
    BY_HANDLE_FILE_INFORMATION native{};
    if (GetFileInformationByHandle(handle, &native) == 0 ||
        (native.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    const auto openedPath = pathFromHandle(handle);
    std::error_code ec;
    if (openedPath.empty() || !std::filesystem::equivalent(expected, openedPath, ec) || ec) {
        return false;
    }
    info.size = (static_cast<std::uint64_t>(native.nFileSizeHigh) << 32) |
        native.nFileSizeLow;
    const std::uint64_t writeTicks =
        (static_cast<std::uint64_t>(native.ftLastWriteTime.dwHighDateTime) << 32) |
        native.ftLastWriteTime.dwLowDateTime;
    constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;
    info.modifiedSeconds = writeTicks >= kUnixEpochTicks
        ? static_cast<std::int64_t>((writeTicks - kUnixEpochTicks) / 10000000ULL) : 0;
    info.modifiedIdentity = writeTicks;
    info.fileIdentity = (static_cast<std::uint64_t>(native.dwVolumeSerialNumber) << 32) ^
        (static_cast<std::uint64_t>(native.nFileIndexHigh) << 16) ^ native.nFileIndexLow;
    return true;
#else
    struct stat opened{};
    struct stat current{};
    if (fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        stat(expected.c_str(), &current) != 0 ||
        opened.st_dev != current.st_dev || opened.st_ino != current.st_ino ||
        opened.st_size < 0) {
        return false;
    }
    info.size = static_cast<std::uint64_t>(opened.st_size);
    info.modifiedSeconds = static_cast<std::int64_t>(opened.st_mtim.tv_sec);
    info.modifiedIdentity =
        static_cast<std::uint64_t>(opened.st_mtim.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(opened.st_mtim.tv_nsec);
    info.fileIdentity = static_cast<std::uint64_t>(opened.st_dev) ^
        (static_cast<std::uint64_t>(opened.st_ino) * 1099511628211ULL);
    return true;
#endif
}

std::ptrdiff_t readFileAt(int descriptor, std::uint64_t offset,
    void* buffer, std::size_t size) {
#ifdef _WIN32
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<__int64>::max()) ||
        size > std::numeric_limits<unsigned int>::max() ||
        _lseeki64(descriptor, static_cast<__int64>(offset), SEEK_SET) < 0) {
        return -1;
    }
    return _read(descriptor, buffer, static_cast<unsigned int>(size));
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return -1;
    return pread(descriptor, buffer, size, static_cast<off_t>(offset));
#endif
}
