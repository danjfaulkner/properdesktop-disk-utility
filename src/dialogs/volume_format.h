// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/signal.h"
#include "base/weak_ptr.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace udisks {
class Operations;
}

// The filesystems a volume can be erased to, and the rules for their names.
namespace dialogs {

inline constexpr uint64_t kMiB = 1024 * 1024;
// Sizes typed without a unit are in GB, as formatSize() shows them.
inline constexpr uint64_t kGB = 1000 * 1000 * 1000;

// UDisks2's names, in the order the Format choices list them: "ext4",
// "exfat", "vfat", "ntfs", "btrfs", "xfs".
const std::vector<std::string>& erasableFilesystems();
// "ext4", "exFAT", "FAT32 (MS-DOS)", "NTFS", "Btrfs", "XFS".
std::string formatName(const std::string& type);
// The partition type for a filesystem in a partition table ("gpt", "dos"):
// Microsoft basic data (0x0c for FAT32, 0x07 for exFAT and NTFS) for the
// filesystems other systems read, Linux data (0x83) for the rest and for
// anything encrypted; empty without a table.
std::string partitionTypeFor(const std::string& tableType, const std::string& filesystem, bool encrypted);
// The longest name the filesystem keeps: 11 for FAT32 and exFAT, 32 for
// NTFS, 16 for ext4 (and ext2, ext3), 15 for swap, 12 for XFS, 255 for
// Btrfs. Counted in characters for FAT32, exFAT and NTFS, in bytes of UTF-8
// for the others.
size_t maximumLabelLength(const std::string& type);
// Why the name cannot be given to the filesystem; empty when it can. An
// empty name always can.
std::string labelProblem(const std::string& type, const std::string& label);
// The name as the filesystem keeps it: FAT32 names in capitals.
std::string normalizedLabel(const std::string& type, const std::string& label);
// "Untitled", in capitals for FAT32.
std::string defaultLabel(const std::string& type);
// The filesystem's root can be given to the user who makes it: ext4, XFS,
// Btrfs.
bool hasOwners(const std::string& type);
// A GUID partition map keeps 36 UTF-16 code units of a partition's name;
// the name cut there on a character boundary.
std::string partitionNameFor(const std::string& name);
// The smallest partition its tools make the filesystem on.
uint64_t minimumSize(const std::string& type);

// Down to a whole MiB.
inline uint64_t alignDown(uint64_t bytes)
{
    return bytes / kMiB * kMiB;
}
inline uint64_t alignUp(uint64_t bytes)
{
    return (bytes + kMiB - 1) / kMiB * kMiB;
}

// Which of the erasable filesystems have their tools installed, as UDisks2
// answers CanFormat for each.
class FormatAvailability
{
public:
    enum class State {
        Unknown,
        Available,
        Missing,
    };
    struct Entry
    {
        State state = State::Unknown;
        // The missing tool, such as "mkfs.xfs".
        std::string missing;
    };

    explicit FormatAvailability(udisks::Operations& operations);

    // Asks for every filesystem; changed follows each answer.
    void query();
    Entry entry(const std::string& type) const;
    bool isAvailable(const std::string& type) const { return entry(type).state == State::Available; }
    // Why the filesystem cannot be chosen; empty when it can.
    std::string reason(const std::string& type) const;

    properdesktop::base::Signal<> changed;

private:
    udisks::Operations& m_operations;
    std::map<std::string, Entry> m_entries;
    properdesktop::base::WeakPtrFactory<FormatAvailability> m_weakFactory{this};
};

} // namespace dialogs
