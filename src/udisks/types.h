// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "base/dbus_types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// UDisks2's objects as plain values, read from its D-Bus properties. Object
// paths are strings, empty where UDisks2 says "/" (none).
namespace udisks {

using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

inline constexpr const char* kService = "org.freedesktop.UDisks2";
inline constexpr const char* kRoot = "/org/freedesktop/UDisks2";
inline constexpr const char* kManagerPath = "/org/freedesktop/UDisks2/Manager";

namespace interface {
inline constexpr const char* kManager = "org.freedesktop.UDisks2.Manager";
inline constexpr const char* kDrive = "org.freedesktop.UDisks2.Drive";
inline constexpr const char* kAta = "org.freedesktop.UDisks2.Drive.Ata";
inline constexpr const char* kNvmeController = "org.freedesktop.UDisks2.NVMe.Controller";
inline constexpr const char* kBlock = "org.freedesktop.UDisks2.Block";
inline constexpr const char* kPartitionTable = "org.freedesktop.UDisks2.PartitionTable";
inline constexpr const char* kPartition = "org.freedesktop.UDisks2.Partition";
inline constexpr const char* kFilesystem = "org.freedesktop.UDisks2.Filesystem";
inline constexpr const char* kEncrypted = "org.freedesktop.UDisks2.Encrypted";
inline constexpr const char* kLoop = "org.freedesktop.UDisks2.Loop";
inline constexpr const char* kSwapspace = "org.freedesktop.UDisks2.Swapspace";
inline constexpr const char* kJob = "org.freedesktop.UDisks2.Job";
} // namespace interface

struct Drive
{
    std::string vendor;
    std::string model;
    std::string revision;
    std::string serial;
    std::string wwn;
    std::string id;
    std::string media;
    std::vector<std::string> mediaCompatibility;
    bool mediaRemovable = false;
    bool mediaAvailable = false;
    uint64_t size = 0;
    bool optical = false;
    bool opticalBlank = false;
    // Revolutions per minute; 0 for a solid-state drive, -1 unknown.
    int32_t rotationRate = -1;
    // "usb", "sdio", "ieee1394" or empty.
    std::string connectionBus;
    std::string seat;
    bool removable = false;
    bool ejectable = false;
    bool canPowerOff = false;
    std::string sortKey;
    std::string siblingId;
    // Microseconds since the epoch: when the drive, and its medium, appeared.
    uint64_t timeDetected = 0;
    uint64_t timeMediaDetected = 0;
};

// S.M.A.R.T. of an ATA drive.
struct Ata
{
    bool smartSupported = false;
    bool smartEnabled = false;
    // Seconds since the epoch; 0 before the data was read.
    uint64_t smartUpdated = 0;
    bool smartFailing = false;
    uint64_t smartPowerOnSeconds = 0;
    // Kelvin; 0 unknown.
    double smartTemperature = 0;
    int32_t smartNumAttributesFailing = -1;
    int32_t smartNumAttributesFailedInThePast = -1;
    int64_t smartNumBadSectors = -1;
    std::string smartSelftestStatus;
    int32_t smartSelftestPercentRemaining = -1;
};

// An NVMe drive's controller and its health log.
struct NvmeController
{
    std::string state;
    std::string revision;
    uint64_t smartUpdated = 0;
    // Empty while healthy: "spare", "temperature", "degraded", "readonly",
    // "volatile_mem", "pmr_readonly".
    std::vector<std::string> smartCriticalWarning;
    uint64_t smartPowerOnHours = 0;
    // Kelvin; 0 unknown.
    uint16_t smartTemperature = 0;
    std::string smartSelftestStatus;
    int32_t smartSelftestPercentRemaining = -1;
};

struct Block
{
    std::string device;
    std::string preferredDevice;
    std::vector<std::string> symlinks;
    // makedev(major, minor).
    uint64_t deviceNumber = 0;
    std::string id;
    uint64_t size = 0;
    bool readOnly = false;
    std::string drive;
    std::string mdRaid;
    std::string mdRaidMember;
    // "filesystem", "crypto", "raid", "other" or empty.
    std::string idUsage;
    std::string idType;
    std::string idVersion;
    std::string idLabel;
    std::string idUuid;
    // The encrypted block this cleartext block unlocks.
    std::string cryptoBackingDevice;
    bool hintPartitionable = false;
    bool hintSystem = false;
    bool hintIgnore = false;
    bool hintAuto = false;
    std::string hintName;
    std::string hintIconName;
    std::string hintSymbolicIconName;
    // It has entries in /etc/fstab or /etc/crypttab.
    bool configured = false;
};

struct PartitionTable
{
    std::vector<std::string> partitions;
    // "gpt" or "dos".
    std::string type;
};

struct Partition
{
    uint32_t number = 0;
    // A GUID on GPT, a hexadecimal byte ("0x83") on MBR.
    std::string type;
    uint64_t flags = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string name;
    std::string uuid;
    std::string table;
    // An MBR extended partition.
    bool isContainer = false;
    // A logical partition inside one.
    bool isContained = false;
};

struct Filesystem
{
    std::vector<std::string> mountPoints;
    // As the filesystem reports it; 0 unknown.
    uint64_t size = 0;
};

struct Encrypted
{
    std::string cleartextDevice;
    // "luks1", "luks2", "bitlk", "tcrypt" and so on.
    std::string hintEncryptionType;
    uint64_t metadataSize = 0;
    // Its cleartext block, while locked, has entries in /etc/fstab.
    bool childConfigured = false;
};

struct Loop
{
    std::string backingFile;
    bool autoclear = false;
    uint32_t setupByUid = 0;
};

struct Swapspace
{
    bool active = false;
};

struct Job
{
    // "format-mkfs", "filesystem-check", "partition-delete" and so on.
    std::string operation;
    // From 0 to 1, while progressValid.
    double progress = 0;
    bool progressValid = false;
    uint64_t bytes = 0;
    // Bytes per second; 0 unknown.
    uint64_t rate = 0;
    // Microseconds since the epoch; 0 unknown.
    uint64_t startTime = 0;
    uint64_t expectedEndTime = 0;
    // The objects the job works on.
    std::vector<std::string> objects;
    uint32_t startedByUid = 0;
    bool cancelable = false;
};

struct Manager
{
    std::string version;
    std::vector<std::string> supportedFilesystems;
    std::vector<std::string> supportedEncryptionTypes;
    std::string defaultEncryptionType;
};

// One exported object and the interfaces it has, both parsed and as received
// (for showing every property).
struct Object
{
    std::string path;
    std::optional<Drive> drive;
    std::optional<Ata> ata;
    std::optional<NvmeController> nvme;
    std::optional<Block> block;
    std::optional<PartitionTable> partitionTable;
    std::optional<Partition> partition;
    std::optional<Filesystem> filesystem;
    std::optional<Encrypted> encrypted;
    std::optional<Loop> loop;
    std::optional<Swapspace> swapspace;
    std::optional<Job> job;
    std::map<std::string, VariantMap> properties;
};

// Fills the parsed member of the interface from its properties, or clears it
// when the properties are gone; unknown interfaces are kept raw only.
void parseInterface(Object& object, const std::string& interface, const VariantMap* properties);
Manager parseManager(const VariantMap& properties);

// A NUL-terminated byte string, as UDisks2 sends paths.
std::string decodeBytes(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encodeBytes(const std::string& text);

} // namespace udisks
