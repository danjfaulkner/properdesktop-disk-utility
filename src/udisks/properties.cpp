// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/properties.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/smart.h"

#include "base/i18n.h"
#include "base/locale.h"

#include <sys/sysmacros.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

using properdesktop::base::dbus::ObjectPath;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using properdesktop::i18n::trc;
using properdesktop::i18n::trNoop;

namespace udisks {

static constexpr const char* kNothing = "—";

std::string formatCount(uint64_t count)
{
    return properdesktop::base::Locale::system().toString(int64_t(count));
}

std::string formatBytes(uint64_t bytes)
{
    const std::string counted = format(properdesktop::i18n::trn("{n} byte", "{n} bytes", bytes),
                                       {{"n", formatCount(bytes)}});
    if (bytes < 1000)
        return counted;
    return format(tr("{size} ({bytes})"), {{"size", formatSize(bytes)}, {"bytes", counted}});
}

std::string formatDate(int64_t secondsSinceEpoch)
{
    if (secondsSinceEpoch <= 0)
        return {};
    const properdesktop::base::Locale& locale = properdesktop::base::Locale::system();
    return locale.toString(properdesktop::base::DateTime::fromSecondsSinceEpoch(secondsSinceEpoch),
                           properdesktop::base::Locale::FormatType::Short);
}

struct NamedType
{
    const char* type;
    const char* name;
};

static constexpr NamedType kGptTypes[] = {
    {"c12a7328-f81f-11d2-ba4b-00a0c93ec93b", trNoop("EFI System")},
    {"21686148-6449-6e6f-744e-656564454649", trNoop("BIOS Boot")},
    {"0fc63daf-8483-4772-8e79-3d69d8477de4", trNoop("Linux Filesystem")},
    {"4f68bce3-e8cd-4db1-96e7-fbcaf984b709", trNoop("Linux Root (x86-64)")},
    {"b921b045-1df0-41c3-af44-4c6f280d3fae", trNoop("Linux Root (ARM64)")},
    {"933ac7e1-2eb4-4f13-b844-0e14e2aef915", trNoop("Linux Home")},
    {"bc13c2ff-59e6-4262-a352-b275fd6f7172", trNoop("Linux Extended Boot")},
    {"0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", trNoop("Linux Swap")},
    {"e6d6d379-f507-44c2-a23c-238f2a3df928", trNoop("Linux LVM")},
    {"a19d880f-05fc-4d3b-a006-743f0f84911e", trNoop("Linux RAID")},
    {"ca7d7ccb-63ed-4c53-861c-1742536059cc", trNoop("Linux LUKS")},
    {"ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", trNoop("Microsoft Basic Data")},
    {"e3c9e316-0b5c-4db8-817d-f92df00215ae", trNoop("Microsoft Reserved")},
    {"de94bba4-06d1-4d40-a16a-bfd50179d6ac", trNoop("Windows Recovery")},
    {"48465300-0000-11aa-aa11-00306543ecac", trNoop("Apple HFS+")},
    {"7c3457ef-0000-11aa-aa11-00306543ecac", trNoop("Apple APFS")},
};

static constexpr NamedType kMbrTypes[] = {
    {"0x01", trNoop("FAT12")},
    {"0x04", trNoop("FAT16")},
    {"0x05", trNoop("Extended")},
    {"0x06", trNoop("FAT16")},
    {"0x07", trNoop("NTFS or exFAT")},
    {"0x0b", trNoop("FAT32")},
    {"0x0c", trNoop("FAT32 (LBA)")},
    {"0x0e", trNoop("FAT16 (LBA)")},
    {"0x0f", trNoop("Extended (LBA)")},
    {"0x82", trNoop("Linux Swap")},
    {"0x83", trNoop("Linux")},
    {"0x85", trNoop("Linux Extended")},
    {"0x8e", trNoop("Linux LVM")},
    {"0xda", trNoop("Non-Filesystem Data")},
    {"0xee", trNoop("GPT Protective")},
    {"0xef", trNoop("EFI System")},
    {"0xfd", trNoop("Linux RAID")},
};

static std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string partitionTypeName(const std::string& tableType, const std::string& type)
{
    std::string key = lower(type);
    if (tableType == "dos") {
        // UDisks2 writes "0x7" as well as "0x07".
        if (key.size() == 3 && key.starts_with("0x"))
            key.insert(2, "0");
        for (const NamedType& known : kMbrTypes) {
            if (key == known.type)
                return tr(known.name);
        }
        return {};
    }
    for (const NamedType& known : kGptTypes) {
        if (key == known.type)
            return tr(known.name);
    }
    return {};
}

std::string partitionFlagsText(const std::string& tableType, uint64_t flags)
{
    std::vector<std::string> names;
    if (tableType == "dos") {
        if (flags & 0x80)
            names.push_back(tr("Bootable"));
    } else {
        if (flags & (uint64_t(1) << 0))
            names.push_back(trc("partition flag", "Required"));
        if (flags & (uint64_t(1) << 1))
            names.push_back(tr("Ignored by Firmware"));
        if (flags & (uint64_t(1) << 2))
            names.push_back(tr("Legacy BIOS Bootable"));
        if (flags & (uint64_t(1) << 60))
            names.push_back(tr("Read Only"));
        if (flags & (uint64_t(1) << 62))
            names.push_back(tr("Hidden"));
        if (flags & (uint64_t(1) << 63))
            names.push_back(tr("No Automount"));
    }
    if (names.empty() && flags != 0) {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(flags));
        return hex;
    }
    if (names.empty())
        return tr("None");
    std::string text;
    for (const std::string& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text;
}

struct NamedProperty
{
    const char* name;
    const char* title;
};

static constexpr NamedProperty kTitles[] = {
    {"AamEnabled", trNoop("Acoustic Management Enabled")},
    {"AamSupported", trNoop("Acoustic Management Supported")},
    {"AamVendorRecommendedValue", trNoop("Acoustic Management Recommended Value")},
    {"Active", trNoop("Active")},
    {"ApmEnabled", trNoop("Power Management (APM) Enabled")},
    {"ApmSupported", trNoop("Power Management (APM) Supported")},
    {"Autoclear", trNoop("Detaches When Unused")},
    {"BackingFile", trNoop("Image File")},
    {"CanPowerOff", trNoop("Can Power Off")},
    {"ChildConfiguration", trNoop("Unlocked Configuration")},
    {"CleartextDevice", trNoop("Unlocked Device")},
    {"Configuration", trNoop("Configuration")},
    {"ConnectionBus", trNoop("Connection")},
    {"ControllerID", trNoop("Controller ID")},
    {"CryptoBackingDevice", trNoop("Encrypted Device")},
    {"Device", trNoop("Device")},
    {"DeviceNumber", trNoop("Device Number")},
    {"Drive", trNoop("Drive")},
    {"Ejectable", trNoop("Ejectable")},
    {"FGUID", trNoop("FRU GUID")},
    {"Flags", trNoop("Flags")},
    {"HintAuto", trNoop("Mounts Automatically")},
    {"HintEncryptionType", trNoop("Encryption")},
    {"HintIconName", trNoop("Icon")},
    {"HintIgnore", trNoop("Hidden from Users")},
    {"HintName", trNoop("Suggested Name")},
    {"HintPartitionable", trNoop("Partitionable")},
    {"HintSymbolicIconName", trNoop("Symbolic Icon")},
    {"HintSystem", trNoop("System Device")},
    {"Id", trNoop("Identifier")},
    {"IdLabel", trNoop("Label")},
    {"IdType", trNoop("Format")},
    {"IdUUID", trNoop("UUID")},
    {"IdUsage", trNoop("Contents")},
    {"IdVersion", trNoop("Format Version")},
    {"IsContained", trNoop("Logical Partition")},
    {"IsContainer", trNoop("Extended Partition")},
    {"MDRaid", trNoop("RAID Array")},
    {"MDRaidMember", trNoop("RAID Member Of")},
    {"Media", trNoop("Media")},
    {"MediaAvailable", trNoop("Media Present")},
    {"MediaChangeDetected", trNoop("Media Change Detected")},
    {"MediaCompatibility", trNoop("Compatible Media")},
    {"MediaRemovable", trNoop("Removable Media")},
    {"MetadataSize", trNoop("Header Size")},
    {"Model", trNoop("Model")},
    {"MountPoints", trNoop("Mount Points")},
    {"NVMeRevision", trNoop("NVMe Version")},
    {"Name", trNoop("Name")},
    {"Number", trNoop("Number")},
    {"Offset", trNoop("Offset")},
    {"Optical", trNoop("Optical")},
    {"OpticalBlank", trNoop("Blank Disc")},
    {"OpticalNumAudioTracks", trNoop("Audio Tracks")},
    {"OpticalNumDataTracks", trNoop("Data Tracks")},
    {"OpticalNumSessions", trNoop("Sessions")},
    {"OpticalNumTracks", trNoop("Tracks")},
    {"Partitions", trNoop("Partitions")},
    {"PmEnabled", trNoop("Power Management Enabled")},
    {"PmSupported", trNoop("Power Management Supported")},
    {"PreferredDevice", trNoop("Preferred Device")},
    {"ReadLookaheadEnabled", trNoop("Read-Ahead Enabled")},
    {"ReadLookaheadSupported", trNoop("Read-Ahead Supported")},
    {"ReadOnly", trNoop("Read Only")},
    {"Removable", trNoop("Removable")},
    {"Revision", trNoop("Firmware Revision")},
    {"RotationRate", trNoop("Rotation Rate")},
    {"SanitizePercentRemaining", trNoop("Sanitize Remaining")},
    {"SanitizeStatus", trNoop("Sanitize Status")},
    {"Seat", trNoop("Seat")},
    {"SecurityEnhancedEraseUnitMinutes", trNoop("Enhanced Secure Erase Time")},
    {"SecurityEraseUnitMinutes", trNoop("Secure Erase Time")},
    {"SecurityFrozen", trNoop("Security Frozen")},
    {"Serial", trNoop("Serial Number")},
    {"SetupByUID", trNoop("Attached by User")},
    {"SiblingId", trNoop("Sibling Identifier")},
    {"Size", trNoop("Size")},
    {"SmartCriticalWarning", trNoop("Critical Warnings")},
    {"SmartEnabled", trNoop("S.M.A.R.T. Enabled")},
    {"SmartFailing", trNoop("Predicts Failure")},
    {"SmartNumAttributesFailedInThePast", trNoop("Attributes Failed in the Past")},
    {"SmartNumAttributesFailing", trNoop("Attributes Failing")},
    {"SmartNumBadSectors", trNoop("Bad Sectors")},
    {"SmartPowerOnHours", trNoop("Powered On")},
    {"SmartPowerOnSeconds", trNoop("Powered On")},
    {"SmartSelftestPercentRemaining", trNoop("Self-Test Remaining")},
    {"SmartSelftestStatus", trNoop("Last Self-Test")},
    {"SmartSupported", trNoop("S.M.A.R.T. Supported")},
    {"SmartTemperature", trNoop("Temperature")},
    {"SmartUpdated", trNoop("S.M.A.R.T. Read")},
    {"SortKey", trNoop("Sort Key")},
    {"State", trNoop("State")},
    {"SubsystemNQN", trNoop("Subsystem NQN")},
    {"Symlinks", trNoop("Links")},
    {"Table", trNoop("Partition Map")},
    {"TimeDetected", trNoop("Detected")},
    {"TimeMediaDetected", trNoop("Media Detected")},
    {"Type", trNoop("Type")},
    {"UUID", trNoop("UUID")},
    {"UnallocatedCapacity", trNoop("Unallocated Capacity")},
    {"UserspaceMountOptions", trNoop("Mount Options")},
    {"Vendor", trNoop("Vendor")},
    {"WWN", trNoop("World Wide Name")},
    {"WriteCacheEnabled", trNoop("Write Cache Enabled")},
    {"WriteCacheSupported", trNoop("Write Cache Supported")},
};

std::string propertyTitle(const std::string& name)
{
    for (const NamedProperty& known : kTitles) {
        if (name == known.name)
            return tr(known.title);
    }
    std::string title;
    for (size_t i = 0; i < name.size(); ++i) {
        const bool upper = std::isupper(static_cast<unsigned char>(name[i]));
        const bool afterLower = i > 0 && std::islower(static_cast<unsigned char>(name[i - 1]));
        const bool beforeLower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1]))
                && i > 0 && std::isupper(static_cast<unsigned char>(name[i - 1]));
        if (upper && (afterLower || beforeLower))
            title += ' ';
        title += name[i];
    }
    return title;
}

std::string interfaceTitle(const std::string& interface)
{
    if (interface == interface::kDrive)
        return tr("Drive");
    if (interface == interface::kAta)
        return tr("S.M.A.R.T.");
    if (interface == interface::kNvmeController)
        return tr("NVMe Controller");
    if (interface == interface::kBlock)
        return tr("Block Device");
    if (interface == interface::kPartitionTable)
        return tr("Partition Map");
    if (interface == interface::kPartition)
        return tr("Partition");
    if (interface == interface::kFilesystem)
        return tr("File System");
    if (interface == interface::kEncrypted)
        return tr("Encryption");
    if (interface == interface::kLoop)
        return tr("Disk Image");
    if (interface == interface::kSwapspace)
        return tr("Swap");
    const size_t dot = interface.rfind('.');
    return dot == std::string::npos ? interface : interface.substr(dot + 1);
}

static std::string joined(const std::vector<std::string>& items, const char* separator)
{
    std::string text;
    for (const std::string& item : items) {
        if (item.empty())
            continue;
        text += (text.empty() ? "" : separator) + item;
    }
    return text;
}

// An object as a person knows it: a block by its device, a drive by its
// vendor and model.
static std::string objectName(const Model& model, const std::string& path)
{
    if (path.empty() || path == "/")
        return {};
    const Object* object = model.object(path);
    if (object && object->block)
        return object->block->device.empty() ? object->block->preferredDevice : object->block->device;
    if (object && object->drive) {
        const std::string name = joined({object->drive->vendor, object->drive->model}, " ");
        return name.empty() ? path.substr(path.rfind('/') + 1) : name;
    }
    return path.substr(path.rfind('/') + 1);
}

static std::string generic(const Model& model, const Variant& value)
{
    const std::string& signature = value.signature();
    if (signature.empty())
        return {};
    switch (signature[0]) {
    case 'b':
        return value.value<bool>() ? tr("Yes") : tr("No");
    case 'y':
        return std::to_string(value.value<uint8_t>());
    case 'n':
        return std::to_string(value.value<int16_t>());
    case 'q':
        return std::to_string(value.value<uint16_t>());
    case 'i':
        return std::to_string(value.value<int32_t>());
    case 'u':
        return std::to_string(value.value<uint32_t>());
    case 'x':
        return std::to_string(value.value<int64_t>());
    case 't':
        return std::to_string(value.value<uint64_t>());
    case 'd': {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value.value<double>());
        return text;
    }
    case 's':
    case 'g':
        return value.value<std::string>();
    case 'o':
        return objectName(model, value.value<ObjectPath>().path);
    case 'v':
        return value.items().empty() ? std::string() : generic(model, value.items().front());
    case 'a': {
        if (signature == "ay")
            return decodeBytes(std::vector<uint8_t>(value.bytes().begin(), value.bytes().end()));
        std::vector<std::string> items;
        for (const Variant& item : value.items())
            items.push_back(generic(model, item));
        // Lists of devices and paths read best a line each.
        const bool lines = signature == "aay" || signature == "ao";
        return joined(items, lines ? "\n" : ", ");
    }
    case '(': {
        std::vector<std::string> items;
        for (const Variant& item : value.items())
            items.push_back(generic(model, item));
        return joined(items, ", ");
    }
    case '{': {
        if (value.items().size() != 2)
            return {};
        const std::string inner = generic(model, value.items()[1]);
        return inner.empty() ? std::string() : generic(model, value.items()[0]) + ": " + inner;
    }
    default:
        return {};
    }
}

template<typename T>
static uint64_t unsignedOf(const Variant& value)
{
    if (const auto number = value.get<T>())
        return uint64_t(*number);
    return 0;
}

static uint64_t anyUnsigned(const Variant& value)
{
    switch (value.signature().empty() ? 0 : value.signature()[0]) {
    case 'y':
        return unsignedOf<uint8_t>(value);
    case 'q':
        return unsignedOf<uint16_t>(value);
    case 'u':
        return unsignedOf<uint32_t>(value);
    case 't':
        return unsignedOf<uint64_t>(value);
    case 'i':
        return uint64_t(std::max<int32_t>(0, value.value<int32_t>()));
    case 'x':
        return uint64_t(std::max<int64_t>(0, value.value<int64_t>()));
    case 'd':
        return uint64_t(std::max(0.0, value.value<double>()));
    default:
        return 0;
    }
}

static std::string tableTypeOf(const Model& model, const Object& object)
{
    if (!object.partition)
        return {};
    const Object* table = model.object(object.partition->table);
    return table && table->partitionTable ? table->partitionTable->type : std::string();
}

static std::string specific(const Model& model, const std::string& interface, const std::string& name,
                            const Variant& value, const Object& object)
{
    if ((name == "Size" || name == "Offset" || name == "MetadataSize" || name == "UnallocatedCapacity")
        && value.signature() == "t") {
        const uint64_t bytes = value.value<uint64_t>();
        return bytes == 0 && name != "Offset" ? std::string() : formatBytes(bytes);
    }
    if (name == "DeviceNumber" && value.signature() == "t") {
        const uint64_t number = value.value<uint64_t>();
        return std::to_string(major(number)) + ":" + std::to_string(minor(number));
    }
    if (name == "TimeDetected" || name == "TimeMediaDetected")
        return formatDate(int64_t(anyUnsigned(value) / 1000000));
    if (name == "SmartUpdated") {
        const uint64_t seconds = anyUnsigned(value);
        return seconds == 0 ? tr("Never") : formatDate(int64_t(seconds));
    }
    if (name == "SmartTemperature")
        return formatTemperature(value.signature() == "d" ? value.value<double>() : double(anyUnsigned(value)));
    if (name == "SmartPowerOnSeconds")
        return anyUnsigned(value) == 0 ? std::string() : formatPowerOnHours(anyUnsigned(value) / 3600);
    if (name == "SmartPowerOnHours")
        return formatPowerOnHours(anyUnsigned(value));
    if (name == "SmartSelftestStatus")
        return selftestStatusText(value.value<std::string>(), -1);
    if (name == "SmartSelftestPercentRemaining" || name == "SanitizePercentRemaining") {
        const int32_t percent = value.value<int32_t>(-1);
        return percent < 0 ? std::string() : std::to_string(percent) + "%";
    }
    if ((name == "SmartNumAttributesFailing" || name == "SmartNumAttributesFailedInThePast"
         || name == "SmartNumBadSectors")
        && (value.signature() == "i" || value.signature() == "x")) {
        const int64_t count = value.signature() == "i" ? value.value<int32_t>() : value.value<int64_t>();
        return count < 0 ? std::string() : formatCount(uint64_t(count));
    }
    if (name == "SmartCriticalWarning" && value.signature() == "as") {
        const std::string warnings = generic(model, value);
        return warnings.empty() ? tr("None") : warnings;
    }
    if (name == "RotationRate" && value.signature() == "i") {
        const int32_t rate = value.value<int32_t>();
        if (rate == 0)
            return tr("Solid State");
        if (rate < 0)
            return tr("Unknown");
        return format(tr("{n} RPM"), {{"n", formatCount(uint64_t(rate))}});
    }
    if (name == "ConnectionBus" && value.signature() == "s") {
        const std::string bus = value.value<std::string>();
        if (bus == "usb")
            return "USB";
        if (bus == "sdio")
            return tr("SD Card");
        if (bus == "ieee1394")
            return "FireWire";
        return bus;
    }
    if (interface == interface::kBlock && name == "IdType")
        return filesystemName(value.value<std::string>(), object.block ? object.block->idVersion : std::string());
    if (interface == interface::kPartitionTable && name == "Type")
        return schemeName(value.value<std::string>());
    if (interface == interface::kPartition && name == "Type") {
        const std::string type = value.value<std::string>();
        const std::string known = partitionTypeName(tableTypeOf(model, object), type);
        return known.empty() ? type : format(tr("{name} ({type})"), {{"name", known}, {"type", type}});
    }
    if (interface == interface::kPartition && name == "Flags")
        return partitionFlagsText(tableTypeOf(model, object), anyUnsigned(value));
    if (name == "HintEncryptionType") {
        std::string type = value.value<std::string>();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return type;
    }
    return generic(model, value);
}

std::string formatProperty(const Model& model, const std::string& interface, const std::string& name,
                           const Variant& value, const Object& object)
{
    const std::string text = specific(model, interface, name, value, object);
    return text.empty() ? kNothing : text;
}

static void addGroups(const Model& model, const Object& object, const std::vector<const char*>& interfaces,
                      std::vector<PropertyGroup>& groups, const std::string& blockTitle = {})
{
    for (const char* interface : interfaces) {
        const auto it = object.properties.find(interface);
        if (it == object.properties.end())
            continue;
        PropertyGroup group;
        group.interface = interface;
        group.title = !blockTitle.empty() && group.interface == interface::kBlock ? blockTitle
                                                                                   : interfaceTitle(interface);
        for (const auto& [name, value] : it->second)
            group.properties.push_back(
                {propertyTitle(name), formatProperty(model, interface, name, value, object), name});
        std::stable_sort(group.properties.begin(), group.properties.end(),
                         [](const Property& a, const Property& b) { return a.name < b.name; });
        groups.push_back(std::move(group));
    }
}

std::vector<PropertyGroup> describeProperties(const Model& model, const Node& node)
{
    std::vector<PropertyGroup> groups;
    const Object* block = model.object(node.block);
    if (!block)
        return groups;
    if (node.kind == NodeKind::Disk) {
        if (const Object* drive = model.object(node.drive))
            addGroups(model, *drive, {interface::kDrive, interface::kAta, interface::kNvmeController}, groups);
        addGroups(model, *block,
                  {interface::kBlock, interface::kPartitionTable, interface::kLoop, interface::kFilesystem}, groups);
        return groups;
    }
    addGroups(model, *block,
              {interface::kBlock, interface::kPartition, interface::kEncrypted, interface::kFilesystem,
               interface::kSwapspace, interface::kPartitionTable},
              groups);
    const Object* content = model.contentOf(node.block);
    if (content && content != block)
        addGroups(model, *content, {interface::kBlock, interface::kFilesystem}, groups, tr("Unlocked Block Device"));
    return groups;
}

} // namespace udisks
