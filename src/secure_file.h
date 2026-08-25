#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

struct OpenedFileInfo {
    std::uint64_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::uint64_t modifiedIdentity = 0;
    std::uint64_t fileIdentity = 0;
};

int openReadFile(const std::filesystem::path& path);
void closeReadFile(int descriptor);
bool inspectReadFile(int descriptor, const std::filesystem::path& expected,
    OpenedFileInfo& info);
std::ptrdiff_t readFileAt(int descriptor, std::uint64_t offset,
    void* buffer, std::size_t size);
