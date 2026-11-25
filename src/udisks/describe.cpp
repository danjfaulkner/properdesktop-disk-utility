// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/describe.h"
#include "udisks/model.h"

#include "base/i18n.h"
#include "base/locale.h"
#include "base/strings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <sys/statvfs.h>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace udisks {

using properdesktop::base::Locale;

std::string formatSize(uint64_t bytes, const Locale& locale)
{
    // The unit is chosen before the rounding: 999.995 GB and up would show
    // as "1,000.00 GB", with a group separator the size fields refuse. They
    // round to 1.00 of the next unit, which is what its first size shows.
    unsigned __int128 unit = 1000;
    while (unit * 1000 <= bytes)
        unit *= 1000;
    if (bytes >= 1000 && (unsigned __int128)(bytes) * 1000 >= unit * 999995)
        return locale.formattedDataSize(int64_t(unit * 1000), 2, Locale::DataSizeFormat::Si);
    return locale.formattedDataSize(int64_t(bytes), 2, Locale::DataSizeFormat::Si);
}

std::string formatSize(uint64_t bytes)
{
    return formatSize(bytes, Locale::system());
}

static std::string asciiLower(std::string_view text)
{
    std::string lower(text);
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// The multiple a unit stands for; 0 for an unknown one. ASCII units in any
// case, and the locale's own as formatSize() writes them.
static uint64_t unitBytes(std::string_view unit, const Locale& locale)
{
    const std::string lower = asciiLower(unit);
    if (lower == "b")
        return 1;
    uint64_t decimal = 1;
    uint64_t binary = 1;
    for (const char letter : std::string_view("kmgt")) {
        decimal *= 1000;
        binary *= 1024;
        const std::string name(1, letter);
        if (lower == name || lower == name + "b")
            return decimal;
        if (lower == name + "ib")
            return binary;
    }
    uint64_t multiple = 1;
    for (int power = 0; power <= 4; ++power, multiple *= 1000) {
        const std::string shown = locale.formattedDataSize(int64_t(multiple), 0, Locale::DataSizeFormat::Si);
        const size_t space = shown.find(' ');
        if (space != std::string::npos && asciiLower(shown.substr(space + 1)) == lower)
            return multiple;
    }
    return 0;
}

// The text with the locale's digits, when they are not ASCII, as ASCII ones.
static std::string asciiDigits(std::string_view text, const Locale& locale)
{
    const std::string zero = locale.zeroDigit();
    std::string result(text);
    if (zero.empty() || zero == "0")
        return result;
    // The ten digits follow the zero in Unicode; the zero's last UTF-8 byte
    // counts up, and none of them crosses a 64-codepoint boundary.
    for (int digit = 9; digit >= 0; --digit) {
        std::string local = zero;
        local.back() = char(static_cast<unsigned char>(local.back()) + digit);
        for (size_t at = result.find(local); at != std::string::npos; at = result.find(local, at + 1))
            result.replace(at, local.size(), 1, char('0' + digit));
    }
    return result;
}

std::optional<uint64_t> parseSize(std::string_view input, uint64_t multiple)
{
    return parseSize(input, multiple, Locale::system());
}

std::optional<uint64_t> parseSize(std::string_view input, uint64_t multiple, const Locale& locale)
{
    const std::string digits = asciiDigits(input, locale);
    const std::string point = locale.decimalPoint().empty() ? std::string(".") : locale.decimalPoint();
    const std::string group = locale.groupSeparator();
    std::string_view text = properdesktop::base::strings::trimmed(digits);
    size_t index = 0;
    unsigned __int128 whole = 0;
    unsigned __int128 fraction = 0;
    unsigned __int128 scale = 1;
    bool any = false;
    bool pointSeen = false;
    while (index < text.size()) {
        const char c = text[index];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            any = true;
            if (pointSeen) {
                // Digits past a byte's worth of precision change nothing.
                if (scale < 1'000'000'000'000'000'000ull) {
                    fraction = fraction * 10 + unsigned(c - '0');
                    scale *= 10;
                }
            } else {
                whole = whole * 10 + unsigned(c - '0');
                if (whole > (unsigned __int128)(1ull << 63))
                    return std::nullopt;
            }
            ++index;
        } else if (!pointSeen && text.substr(index).starts_with(point)) {
            pointSeen = true;
            index += point.size();
        } else if (!group.empty() && group != point && text.substr(index).starts_with(group)) {
            return std::nullopt;
        } else {
            break;
        }
    }
    if (!any)
        return std::nullopt;

    const std::string_view unit = properdesktop::base::strings::trimmed(text.substr(index));
    const uint64_t bytesPerUnit = unit.empty() ? multiple : unitBytes(unit, locale);
    if (bytesPerUnit == 0)
        return std::nullopt;

    const unsigned __int128 bytes = whole * bytesPerUnit + (fraction * bytesPerUnit + scale / 2) / scale;
    if (bytes == 0 || bytes > (unsigned __int128)(1ull << 63))
        return std::nullopt;
    return uint64_t(bytes);
}

std::string deviceName(const Model& model, const std::string& block)
{
    const Object* object = model.object(block);
    if (!object || !object->block)
        return {};
    const std::string& device = object->block->device.empty() ? object->block->preferredDevice : object->block->device;
    return device.starts_with("/dev/") ? device.substr(5) : device;
}

std::string filesystemName(const std::string& type, const std::string& version)
{
    if (type == "vfat") {
        if (version == "FAT12" || version == "FAT16" || version == "FAT32")
            return version;
        return "FAT";
    }
    if (type == "exfat")
        return "exFAT";
    if (type == "ntfs")
        return "NTFS";
    if (type == "btrfs")
        return "Btrfs";
    if (type == "xfs")
        return "XFS";
    if (type == "f2fs")
        return "F2FS";
    if (type == "hfsplus")
        return "HFS+";
    if (type == "iso9660")
        return "ISO 9660";
    if (type == "udf")
        return "UDF";
    if (type == "swap")
        return tr("Swap");
    if (type == "crypto_LUKS")
        return "LUKS";
    if (type == "BitLocker")
        return "BitLocker";
    if (type == "LVM2_member")
        return "LVM";
    if (type == "linux_raid_member")
        return "RAID";
    return type;
}

std::string schemeName(const std::string& tableType)
{
    if (tableType == "gpt")
        return tr("GUID Partition Map");
    if (tableType == "dos")
        return tr("Master Boot Record");
    return {};
}

std::string displayName(const Model& model, const Node& node)
{
    const Object* object = model.object(node.block);
    const Block* block = object && object->block ? &*object->block : nullptr;
    const uint64_t size = block ? block->size : 0;

    switch (node.kind) {
    case NodeKind::Disk: {
        if (object && object->loop && !object->loop->backingFile.empty())
            return std::filesystem::path(object->loop->backingFile).filename().string();
        const Object* drive = model.object(node.drive);
        if (drive && drive->drive) {
            std::string name(properdesktop::base::strings::trimmed(drive->drive->vendor + " " + drive->drive->model));
            if (!name.empty())
                return name;
        }
        // TRANSLATORS: a disk without a model name, such as "16 GB Disk".
        return format(tr("{size} Disk"), {{"size", formatSize(size)}});
    }
    case NodeKind::Container:
        return tr("Extended Partition");
    case NodeKind::Partition:
        if (object && object->partition && !object->partition->name.empty())
            return object->partition->name;
        return deviceName(model, node.block);
    case NodeKind::Volume:
        break;
    }

    if (block && !block->hintName.empty())
        return block->hintName;
    const Object* content = model.contentOf(node.block);
    if (content && content->block && !content->block->idLabel.empty())
        return content->block->idLabel;
    if (block && !block->idLabel.empty())
        return block->idLabel;
    const bool locked = object && object->encrypted && content == object;
    // TRANSLATORS: a volume without a label, named by its size, such as "16 GB Volume".
    return format(locked ? tr("{size} Encrypted Volume") : tr("{size} Volume"), {{"size", formatSize(size)}});
}

std::string iconName(const Model& model, const Node& node)
{
    const Object* object = model.object(node.block);
    if (object && object->block && !object->block->hintIconName.empty())
        return object->block->hintIconName;
    if (node.drive.empty())
        return "media-optical";
    const Object* drive = model.object(node.drive);
    if (drive && drive->drive) {
        if (drive->drive->optical)
            return "drive-optical";
        if (connectionOf(model, node.drive) == Connection::SdCard || drive->drive->media.starts_with("flash"))
            return "media-flash";
        if (sectionOfDrive(model, node.drive) == Section::External)
            return "drive-removable-media";
    }
    return "drive-harddisk";
}

Connection connectionOf(const Model& model, const std::string& drive)
{
    if (drive.empty())
        return Connection::DiskImage;
    const Object* object = model.object(drive);
    if (!object || !object->drive)
        return Connection::Unknown;
    const std::string& bus = object->drive->connectionBus;
    if (bus == "usb")
        return Connection::Usb;
    if (bus == "sdio")
        return Connection::SdCard;
    if (bus == "ieee1394")
        return Connection::FireWire;
    if (object->nvme)
        return Connection::Nvme;
    if (object->ata)
        return Connection::Sata;
    if (const Object* whole = model.wholeBlock(drive); whole && whole->block->device.starts_with("/dev/nvme"))
        return Connection::Nvme;
    return Connection::Unknown;
}

std::string connectionName(Connection connection)
{
    switch (connection) {
    case Connection::Unknown:
        return {};
    case Connection::Sata:
        return "SATA";
    case Connection::Nvme:
        return "NVMe";
    case Connection::Usb:
        return "USB";
    case Connection::SdCard:
        return tr("SD Card");
    case Connection::FireWire:
        return "FireWire";
    case Connection::DiskImage:
        return tr("Disk Image");
    }
    return {};
}

std::optional<FilesystemStats> statFilesystem(const std::string& mountPoint)
{
    struct statvfs info = {};
    if (mountPoint.empty() || ::statvfs(mountPoint.c_str(), &info) != 0)
        return std::nullopt;
    return FilesystemStats{info.f_frsize ? uint64_t(info.f_frsize) : uint64_t(info.f_bsize), uint64_t(info.f_blocks),
                           uint64_t(info.f_bfree), uint64_t(info.f_bavail)};
}

std::string mountPointOf(const Model& model, const Node& node)
{
    if (node.kind != NodeKind::Volume)
        return {};
    const Object* content = model.contentOf(node.block);
    if (!content || !content->filesystem || content->filesystem->mountPoints.empty())
        return {};
    return content->filesystem->mountPoints.front();
}

Usage usageOf(const Model& model, const Node& node, const StatFunction& stat)
{
    Usage usage;
    const Object* object = model.object(node.block);
    if (!object || !object->block)
        return usage;
    usage.capacity = object->block->size;

    if (node.kind == NodeKind::Disk) {
        if (!object->partitionTable) {
            if (object->filesystem || object->encrypted) {
                usage.used = usage.capacity;
                usage.known = true;
            }
            return usage;
        }
        uint64_t partitioned = 0;
        for (const Object* partition : model.partitionsOf(node.block)) {
            if (!partition->partition->isContained)
                partitioned += partition->partition->size;
        }
        usage.used = std::min(partitioned, usage.capacity);
        usage.available = usage.capacity - usage.used;
        usage.known = true;
        return usage;
    }

    const std::string mountPoint = mountPointOf(model, node);
    if (mountPoint.empty() || !stat)
        return usage;
    const std::optional<FilesystemStats> stats = stat(mountPoint);
    if (!stats)
        return usage;
    const uint64_t total = stats->blocks * stats->fragmentSize;
    usage.used = std::min(total - std::min(total, stats->free * stats->fragmentSize), usage.capacity);
    usage.available = std::min(stats->available * stats->fragmentSize, usage.capacity - usage.used);
    usage.known = true;
    return usage;
}

} // namespace udisks
