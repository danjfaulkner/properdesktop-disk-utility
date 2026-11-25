// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/types.h"

namespace udisks {

using properdesktop::base::dbus::ObjectPath;

std::string decodeBytes(const std::vector<uint8_t>& bytes)
{
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && text.back() == '\0')
        text.pop_back();
    return text;
}

std::vector<uint8_t> encodeBytes(const std::string& text)
{
    std::vector<uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}

// The value of a property, which may still sit inside its "v".
static const Variant* find(const VariantMap& map, const char* key)
{
    const auto found = map.find(key);
    if (found == map.end())
        return nullptr;
    const Variant* value = &found->second;
    while (value->signature() == "v" && !value->items().empty())
        value = &value->items().front();
    return value;
}

// An array property with at least one element.
static bool hasItems(const VariantMap& map, const char* key)
{
    const Variant* value = find(map, key);
    return value && value->signature().starts_with("a") && !value->items().empty();
}

template<typename T>
static T get(const VariantMap& map, const char* key, T fallback = T())
{
    const Variant* value = find(map, key);
    if (!value)
        return fallback;
    return value->get<T>().value_or(fallback);
}

// Integers whatever their width or sign, as a property's type has changed
// between versions.
template<typename T>
static T number(const VariantMap& map, const char* key, T fallback = T())
{
    const Variant* value = find(map, key);
    if (!value)
        return fallback;
    if (const auto v = value->get<uint64_t>())
        return T(*v);
    if (const auto v = value->get<int64_t>())
        return T(*v);
    if (const auto v = value->get<uint32_t>())
        return T(*v);
    if (const auto v = value->get<int32_t>())
        return T(*v);
    if (const auto v = value->get<uint16_t>())
        return T(*v);
    if (const auto v = value->get<int16_t>())
        return T(*v);
    if (const auto v = value->get<uint8_t>())
        return T(*v);
    if (const auto v = value->get<double>())
        return T(*v);
    return fallback;
}

static std::string path(const VariantMap& map, const char* key)
{
    const std::string text = get<ObjectPath>(map, key).path;
    return text == "/" ? std::string() : text;
}

static std::vector<std::string> paths(const VariantMap& map, const char* key)
{
    std::vector<std::string> result;
    for (const ObjectPath& item : get<std::vector<ObjectPath>>(map, key)) {
        if (!item.path.empty() && item.path != "/")
            result.push_back(item.path);
    }
    return result;
}

static std::string bytes(const VariantMap& map, const char* key)
{
    return decodeBytes(get<std::vector<uint8_t>>(map, key));
}

static std::vector<std::string> byteStrings(const VariantMap& map, const char* key)
{
    std::vector<std::string> result;
    for (const std::vector<uint8_t>& item : get<std::vector<std::vector<uint8_t>>>(map, key)) {
        std::string text = decodeBytes(item);
        if (!text.empty())
            result.push_back(std::move(text));
    }
    return result;
}

static Drive parseDrive(const VariantMap& map)
{
    Drive drive;
    drive.vendor = get<std::string>(map, "Vendor");
    drive.model = get<std::string>(map, "Model");
    drive.revision = get<std::string>(map, "Revision");
    drive.serial = get<std::string>(map, "Serial");
    drive.wwn = get<std::string>(map, "WWN");
    drive.id = get<std::string>(map, "Id");
    drive.media = get<std::string>(map, "Media");
    drive.mediaCompatibility = get<std::vector<std::string>>(map, "MediaCompatibility");
    drive.mediaRemovable = get<bool>(map, "MediaRemovable");
    drive.mediaAvailable = get<bool>(map, "MediaAvailable");
    drive.size = number<uint64_t>(map, "Size");
    drive.optical = get<bool>(map, "Optical");
    drive.opticalBlank = get<bool>(map, "OpticalBlank");
    drive.rotationRate = number<int32_t>(map, "RotationRate", -1);
    drive.connectionBus = get<std::string>(map, "ConnectionBus");
    drive.seat = get<std::string>(map, "Seat");
    drive.removable = get<bool>(map, "Removable");
    drive.ejectable = get<bool>(map, "Ejectable");
    drive.canPowerOff = get<bool>(map, "CanPowerOff");
    drive.sortKey = get<std::string>(map, "SortKey");
    drive.siblingId = get<std::string>(map, "SiblingId");
    drive.timeDetected = number<uint64_t>(map, "TimeDetected");
    drive.timeMediaDetected = number<uint64_t>(map, "TimeMediaDetected");
    return drive;
}

static Ata parseAta(const VariantMap& map)
{
    Ata ata;
    ata.smartSupported = get<bool>(map, "SmartSupported");
    ata.smartEnabled = get<bool>(map, "SmartEnabled");
    ata.smartUpdated = number<uint64_t>(map, "SmartUpdated");
    ata.smartFailing = get<bool>(map, "SmartFailing");
    ata.smartPowerOnSeconds = number<uint64_t>(map, "SmartPowerOnSeconds");
    ata.smartTemperature = number<double>(map, "SmartTemperature");
    ata.smartNumAttributesFailing = number<int32_t>(map, "SmartNumAttributesFailing", -1);
    ata.smartNumAttributesFailedInThePast = number<int32_t>(map, "SmartNumAttributesFailedInThePast", -1);
    ata.smartNumBadSectors = number<int64_t>(map, "SmartNumBadSectors", -1);
    ata.smartSelftestStatus = get<std::string>(map, "SmartSelftestStatus");
    ata.smartSelftestPercentRemaining = number<int32_t>(map, "SmartSelftestPercentRemaining", -1);
    return ata;
}

static NvmeController parseNvme(const VariantMap& map)
{
    NvmeController nvme;
    nvme.state = get<std::string>(map, "State");
    nvme.revision = get<std::string>(map, "NVMeRevision");
    nvme.smartUpdated = number<uint64_t>(map, "SmartUpdated");
    nvme.smartCriticalWarning = get<std::vector<std::string>>(map, "SmartCriticalWarning");
    nvme.smartPowerOnHours = number<uint64_t>(map, "SmartPowerOnHours");
    nvme.smartTemperature = number<uint16_t>(map, "SmartTemperature");
    nvme.smartSelftestStatus = get<std::string>(map, "SmartSelftestStatus");
    nvme.smartSelftestPercentRemaining = number<int32_t>(map, "SmartSelftestPercentRemaining", -1);
    return nvme;
}

static Block parseBlock(const VariantMap& map)
{
    Block block;
    block.device = bytes(map, "Device");
    block.preferredDevice = bytes(map, "PreferredDevice");
    if (block.preferredDevice.empty())
        block.preferredDevice = block.device;
    block.symlinks = byteStrings(map, "Symlinks");
    block.deviceNumber = number<uint64_t>(map, "DeviceNumber");
    block.id = get<std::string>(map, "Id");
    block.size = number<uint64_t>(map, "Size");
    block.readOnly = get<bool>(map, "ReadOnly");
    block.drive = path(map, "Drive");
    block.mdRaid = path(map, "MDRaid");
    block.mdRaidMember = path(map, "MDRaidMember");
    block.idUsage = get<std::string>(map, "IdUsage");
    block.idType = get<std::string>(map, "IdType");
    block.idVersion = get<std::string>(map, "IdVersion");
    block.idLabel = get<std::string>(map, "IdLabel");
    block.idUuid = get<std::string>(map, "IdUUID");
    block.cryptoBackingDevice = path(map, "CryptoBackingDevice");
    block.hintPartitionable = get<bool>(map, "HintPartitionable");
    block.hintSystem = get<bool>(map, "HintSystem");
    block.hintIgnore = get<bool>(map, "HintIgnore");
    block.hintAuto = get<bool>(map, "HintAuto");
    block.hintName = get<std::string>(map, "HintName");
    block.configured = hasItems(map, "Configuration");
    block.hintIconName = get<std::string>(map, "HintIconName");
    block.hintSymbolicIconName = get<std::string>(map, "HintSymbolicIconName");
    return block;
}

static PartitionTable parsePartitionTable(const VariantMap& map)
{
    PartitionTable table;
    table.partitions = paths(map, "Partitions");
    table.type = get<std::string>(map, "Type");
    return table;
}

static Partition parsePartition(const VariantMap& map)
{
    Partition partition;
    partition.number = number<uint32_t>(map, "Number");
    partition.type = get<std::string>(map, "Type");
    partition.flags = number<uint64_t>(map, "Flags");
    partition.offset = number<uint64_t>(map, "Offset");
    partition.size = number<uint64_t>(map, "Size");
    partition.name = get<std::string>(map, "Name");
    partition.uuid = get<std::string>(map, "UUID");
    partition.table = path(map, "Table");
    partition.isContainer = get<bool>(map, "IsContainer");
    partition.isContained = get<bool>(map, "IsContained");
    return partition;
}

static Filesystem parseFilesystem(const VariantMap& map)
{
    Filesystem filesystem;
    filesystem.mountPoints = byteStrings(map, "MountPoints");
    filesystem.size = number<uint64_t>(map, "Size");
    return filesystem;
}

static Encrypted parseEncrypted(const VariantMap& map)
{
    Encrypted encrypted;
    encrypted.cleartextDevice = path(map, "CleartextDevice");
    encrypted.hintEncryptionType = get<std::string>(map, "HintEncryptionType");
    encrypted.metadataSize = number<uint64_t>(map, "MetadataSize");
    encrypted.childConfigured = hasItems(map, "ChildConfiguration");
    return encrypted;
}

static Loop parseLoop(const VariantMap& map)
{
    Loop loop;
    loop.backingFile = bytes(map, "BackingFile");
    loop.autoclear = get<bool>(map, "Autoclear");
    loop.setupByUid = number<uint32_t>(map, "SetupByUID");
    return loop;
}

static Swapspace parseSwapspace(const VariantMap& map)
{
    Swapspace swapspace;
    swapspace.active = get<bool>(map, "Active");
    return swapspace;
}

static Job parseJob(const VariantMap& map)
{
    Job job;
    job.operation = get<std::string>(map, "Operation");
    job.progress = number<double>(map, "Progress");
    job.progressValid = get<bool>(map, "ProgressValid");
    job.bytes = number<uint64_t>(map, "Bytes");
    job.rate = number<uint64_t>(map, "Rate");
    job.startTime = number<uint64_t>(map, "StartTime");
    job.expectedEndTime = number<uint64_t>(map, "ExpectedEndTime");
    job.objects = paths(map, "Objects");
    job.startedByUid = number<uint32_t>(map, "StartedByUID");
    job.cancelable = get<bool>(map, "Cancelable");
    return job;
}

template<typename T, typename Parse>
static void assign(std::optional<T>& member, const VariantMap* properties, Parse parse)
{
    if (properties)
        member = parse(*properties);
    else
        member.reset();
}

void parseInterface(Object& object, const std::string& name, const VariantMap* properties)
{
    if (name == interface::kDrive)
        assign(object.drive, properties, parseDrive);
    else if (name == interface::kAta)
        assign(object.ata, properties, parseAta);
    else if (name == interface::kNvmeController)
        assign(object.nvme, properties, parseNvme);
    else if (name == interface::kBlock)
        assign(object.block, properties, parseBlock);
    else if (name == interface::kPartitionTable)
        assign(object.partitionTable, properties, parsePartitionTable);
    else if (name == interface::kPartition)
        assign(object.partition, properties, parsePartition);
    else if (name == interface::kFilesystem)
        assign(object.filesystem, properties, parseFilesystem);
    else if (name == interface::kEncrypted)
        assign(object.encrypted, properties, parseEncrypted);
    else if (name == interface::kLoop)
        assign(object.loop, properties, parseLoop);
    else if (name == interface::kSwapspace)
        assign(object.swapspace, properties, parseSwapspace);
    else if (name == interface::kJob)
        assign(object.job, properties, parseJob);
}

Manager parseManager(const VariantMap& map)
{
    Manager manager;
    manager.version = get<std::string>(map, "Version");
    manager.supportedFilesystems = get<std::vector<std::string>>(map, "SupportedFilesystems");
    manager.supportedEncryptionTypes = get<std::vector<std::string>>(map, "SupportedEncryptionTypes");
    manager.defaultEncryptionType = get<std::string>(map, "DefaultEncryptionType");
    return manager;
}

} // namespace udisks
