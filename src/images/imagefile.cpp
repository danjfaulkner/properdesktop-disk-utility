// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "images/imagefile.h"

#include "udisks/model.h"

#include "base/i18n.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace images {

static std::string lowerExtension(const std::string& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension;
}

const std::vector<std::string>& imagePatterns()
{
    static const std::vector<std::string> patterns = {"*.img", "*.iso", "*.raw", "*.iso9660", "*.raw-disk-image"};
    return patterns;
}

bool isDiskImageFile(const std::string& path)
{
    const std::string extension = lowerExtension(path);
    return std::any_of(imagePatterns().begin(), imagePatterns().end(),
                       [&extension](const std::string& pattern) { return pattern.substr(1) == extension; });
}

bool attachesReadOnly(const std::string& path)
{
    const std::string extension = lowerExtension(path);
    return extension == ".iso" || extension == ".iso9660";
}

std::string imageFileName(const std::string& name)
{
    std::string trimmed = name;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
        trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    if (trimmed.empty() || trimmed == "." || trimmed == ".." || trimmed.find('/') != std::string::npos)
        return {};
    if (isDiskImageFile(trimmed))
        return trimmed;
    return trimmed + ".img";
}

static std::string normalized(const std::string& path)
{
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return error ? std::filesystem::path(path).lexically_normal().string() : canonical.string();
}

// The same file: by device and inode, so a hard link or a path through a
// bind mount is one too; by the path when either cannot be looked at.
static bool sameFile(const std::string& path, const std::string& other)
{
    struct stat first {};
    struct stat second {};
    if (::stat(path.c_str(), &first) == 0 && ::stat(other.c_str(), &second) == 0)
        return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
    return normalized(path) == normalized(other);
}

std::string loopOfFile(const udisks::Model& model, const std::string& path)
{
    if (path.empty())
        return {};
    for (const auto& [objectPath, object] : model.objects()) {
        if (object.loop && !object.loop->backingFile.empty() && sameFile(path, object.loop->backingFile))
            return objectPath;
    }
    return {};
}

std::string compressionOf(const std::string& path)
{
    unsigned char head[8] = {};
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    const ssize_t got = ::read(fd, head, sizeof(head));
    ::close(fd);
    if (got <= 0)
        return {};
    struct Signature
    {
        const char* name;
        std::string_view bytes;
    };
    using namespace std::string_view_literals;
    static constexpr Signature kSignatures[] = {
        {"xz", "\xFD" "7zXZ\0"sv},
        {"gzip", "\x1F\x8B"sv},
        {"bzip2", "BZh"sv},
        {"zstd", "\x28\xB5\x2F\xFD"sv},
        {"ZIP", "PK\x03\x04"sv},
        {"7-Zip", "7z\xBC\xAF\x27\x1C"sv},
        {"LZ4", "\x04\x22\x4D\x18"sv},
    };
    const std::string_view start(reinterpret_cast<const char*>(head), size_t(got));
    for (const Signature& signature : kSignatures) {
        if (start.starts_with(signature.bytes))
            return signature.name;
    }
    return {};
}

std::string createBlankFile(const std::string& path, uint64_t size)
{
    // The file takes its room as it is written, so it has to be there already.
    struct statvfs space {};
    const std::string folder = std::filesystem::path(path).parent_path().string();
    if (::statvfs(folder.empty() ? "." : folder.c_str(), &space) == 0
        && uint64_t(space.f_bavail) * space.f_frsize < size)
        return tr("There is not enough free space for an image of that size.");

    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            return tr("A file with that name already exists.");
        return format(tr("The file could not be created: {reason}"), {{"reason", std::strerror(errno)}});
    }
    if (::ftruncate(fd, off_t(size)) != 0) {
        const int error = errno;
        ::close(fd);
        ::unlink(path.c_str());
        if (error == EFBIG)
            return tr("The file is too large for the disk it is on.");
        return format(tr("The file could not be created: {reason}"), {{"reason", std::strerror(error)}});
    }
    ::close(fd);
    return {};
}

} // namespace images
