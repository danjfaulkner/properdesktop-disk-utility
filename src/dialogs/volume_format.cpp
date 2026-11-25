// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/volume_format.h"

#include "udisks/operations.h"

#include "base/i18n.h"

#include <cctype>
#include <string_view>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

const std::vector<std::string>& erasableFilesystems()
{
    static const std::vector<std::string> types = {"ext4", "exfat", "vfat", "ntfs", "btrfs", "xfs"};
    return types;
}

std::string formatName(const std::string& type)
{
    if (type == "ext4")
        return "ext4";
    if (type == "exfat")
        return "exFAT";
    if (type == "vfat")
        return tr("FAT32 (MS-DOS)");
    if (type == "ntfs")
        return "NTFS";
    if (type == "btrfs")
        return "Btrfs";
    if (type == "xfs")
        return "XFS";
    if (type == "swap")
        return tr("Swap");
    return type;
}

std::string partitionTypeFor(const std::string& tableType, const std::string& filesystem, bool encrypted)
{
    const bool shared = !encrypted && (filesystem == "vfat" || filesystem == "exfat" || filesystem == "ntfs");
    if (tableType == "gpt")
        return shared ? "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" : "0fc63daf-8483-4772-8e79-3d69d8477de4";
    if (tableType == "dos")
        return !shared ? "0x83" : filesystem == "vfat" ? "0x0c" : "0x07";
    return {};
}

static bool countsCharacters(const std::string& type)
{
    return type == "vfat" || type == "exfat" || type == "ntfs";
}

size_t maximumLabelLength(const std::string& type)
{
    // What mkfs.fat and mkfs.exfat take, and the 32 characters NTFS names
    // are held to; the others are the superblock's field.
    if (type == "vfat" || type == "exfat")
        return 11;
    if (type == "ntfs")
        return 32;
    if (type == "ext4" || type == "ext3" || type == "ext2")
        return 16;
    if (type == "swap")
        return 15;
    if (type == "xfs")
        return 12;
    if (type == "btrfs")
        return 255;
    return 255;
}

// In UTF-16 code units, as FAT32, exFAT and NTFS store names.
static size_t characterCount(const std::string& text)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xc0) == 0x80)
            continue;
        count += byte >= 0xf0 ? 2 : 1;
    }
    return count;
}

std::string labelProblem(const std::string& type, const std::string& label)
{
    if (label.empty())
        return {};
    for (const char c : label) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f)
            return tr("The name can't contain control characters.");
    }
    if (type == "vfat") {
        static constexpr std::string_view kForbidden = "\"*+,./:;<=>?[\\]|";
        for (const char c : label) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte >= 0x80)
                return tr("FAT32 names can only use unaccented letters, digits and simple punctuation.");
            if (kForbidden.find(c) != std::string_view::npos)
                return format(tr("FAT32 names can't contain “{character}”."), {{"character", std::string(1, c)}});
        }
    }
    const size_t maximum = maximumLabelLength(type);
    const size_t length = countsCharacters(type) ? characterCount(label) : label.size();
    if (length > maximum) {
        const std::string count = std::to_string(maximum);
        return countsCharacters(type)
                ? format(tr("{format} names can have at most {count} characters."),
                         {{"format", formatName(type)}, {"count", count}})
                : format(tr("{format} names can have at most {count} bytes."),
                         {{"format", formatName(type)}, {"count", count}});
    }
    return {};
}

std::string normalizedLabel(const std::string& type, const std::string& label)
{
    if (type != "vfat")
        return label;
    std::string upper = label;
    for (char& c : upper)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}

std::string defaultLabel(const std::string& type)
{
    return normalizedLabel(type, tr("Untitled"));
}

bool hasOwners(const std::string& type)
{
    return type == "ext4" || type == "xfs" || type == "btrfs";
}

std::string partitionNameFor(const std::string& name)
{
    size_t units = 0;
    size_t i = 0;
    while (i < name.size()) {
        const auto byte = static_cast<unsigned char>(name[i]);
        const size_t length = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;
        const size_t needed = length == 4 ? 2 : 1;
        if (units + needed > 36)
            break;
        units += needed;
        i += length;
    }
    return name.substr(0, i);
}

uint64_t minimumSize(const std::string& type)
{
    // FAT32 needs 65525 clusters, Btrfs and XFS refuse smaller devices in
    // their current tools.
    if (type == "vfat")
        return 33 * kMiB;
    if (type == "btrfs")
        return 128 * kMiB;
    if (type == "xfs")
        return 300 * kMiB;
    return 8 * kMiB;
}

FormatAvailability::FormatAvailability(udisks::Operations& operations)
    : m_operations(operations)
{
}

void FormatAvailability::query()
{
    for (const std::string& type : erasableFilesystems()) {
        m_operations.canFormat(type, [self = m_weakFactory.getWeakPtr(), type](bool available,
                                                                                const std::string& missing) {
            FormatAvailability* availability = self.get();
            if (!availability)
                return;
            availability->m_entries[type] = {available ? State::Available : State::Missing, missing};
            availability->changed.emit();
        });
    }
}

FormatAvailability::Entry FormatAvailability::entry(const std::string& type) const
{
    const auto it = m_entries.find(type);
    return it == m_entries.end() ? Entry{} : it->second;
}

std::string FormatAvailability::reason(const std::string& type) const
{
    const Entry found = entry(type);
    switch (found.state) {
    case State::Available:
        return {};
    case State::Unknown:
        return tr("Checking for its tools…");
    case State::Missing:
        if (found.missing.empty())
            return tr("Its tools are not installed.");
        return format(tr("Needs {tool}, which is not installed."), {{"tool", found.missing}});
    }
    return {};
}

} // namespace dialogs
