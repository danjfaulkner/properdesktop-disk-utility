// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/tree.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace properdesktop::base {
class Locale;
}

// What the sidebar, the detail pane and the dialogs say about a node.
namespace udisks {

class Model;

// "Samsung SSD 980" for a disk ("{size} Disk" without a model), the image
// file's name for a disk image, the label for a volume ("{size} Volume"
// without one), the partition's name or device for a partition.
std::string displayName(const Model& model, const Node& node);
// The block device's name without /dev/: "nvme0n1p2".
std::string deviceName(const Model& model, const std::string& block);
// "512.11 GB": decimal units, as drives are sold.
std::string formatSize(uint64_t bytes);
std::string formatSize(uint64_t bytes, const properdesktop::base::Locale& locale);
// A size typed by the user, in `multiple` bytes unless a unit follows: the
// units formatSize() writes in the locale ("512 MB", "1,5 Go") or the ASCII
// ones ("1.5G", "2 GiB", "4096 B"; decimal, binary with "i"). The number
// takes the locale's digits and its decimal separator only; a grouping
// separator is refused rather than guessed at, so "1.500" in German is no
// size. None for anything else, zero, or a size past 2^63 bytes.
std::optional<uint64_t> parseSize(std::string_view text, uint64_t multiple);
std::optional<uint64_t> parseSize(std::string_view text, uint64_t multiple, const properdesktop::base::Locale& locale);
// The common name of UDisks2's IdType: "ext4", "FAT32", "exFAT", "NTFS",
// "Btrfs", "XFS", "Swap", "LUKS", "ISO 9660"; the IdType itself otherwise,
// empty for none.
std::string filesystemName(const std::string& idType, const std::string& idVersion = {});
// "GUID Partition Map" or "Master Boot Record"; empty for none.
std::string schemeName(const std::string& tableType);
// A theme icon name for the node: drive-harddisk, drive-removable-media,
// drive-optical, media-flash...
std::string iconName(const Model& model, const Node& node);

enum class Connection {
    Unknown,
    Sata,
    Nvme,
    Usb,
    SdCard,
    FireWire,
    DiskImage,
};

// How the drive is attached; DiskImage for an empty drive path.
Connection connectionOf(const Model& model, const std::string& drive);
// "SATA", "NVMe", "USB", "SD Card", "FireWire", "Disk Image"; empty unknown.
std::string connectionName(Connection connection);

// What statvfs says of a mounted filesystem.
struct FilesystemStats
{
    uint64_t fragmentSize = 0;
    uint64_t blocks = 0;
    // Free, the root's reserve included.
    uint64_t free = 0;
    // Free for anyone.
    uint64_t available = 0;
};
using StatFunction = std::function<std::optional<FilesystemStats>(const std::string& mountPoint)>;
std::optional<FilesystemStats> statFilesystem(const std::string& mountPoint);

// The usage bar's numbers. For a volume: its block's size, and what the
// filesystem says it uses and has available while it is mounted; the rest
// (the root's reserve, metadata) is other(). For a disk or disk image:
// partitioned space as used, unpartitioned space as available.
struct Usage
{
    uint64_t capacity = 0;
    uint64_t used = 0;
    uint64_t available = 0;
    // The numbers past capacity are known: false for a volume that is not
    // mounted, or a partition without a filesystem.
    bool known = false;

    uint64_t other() const
    {
        const uint64_t taken = used + available;
        return taken >= capacity ? 0 : capacity - taken;
    }
};
Usage usageOf(const Model& model, const Node& node, const StatFunction& stat = statFilesystem);

// The first mount point of the node's filesystem (through its cleartext
// block for an encrypted one); empty when it is not mounted.
std::string mountPointOf(const Model& model, const Node& node);

} // namespace udisks
