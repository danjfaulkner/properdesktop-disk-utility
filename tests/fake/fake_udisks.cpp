// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "base/event_loop.h"

#include <sys/sysmacros.h>

namespace udisks::fake {

using properdesktop::base::EventLoop;
using properdesktop::base::dbus::ObjectPath;

static void post(Done done, Result result)
{
    EventLoop::current()->post([done = std::move(done), result = std::move(result)] {
        if (done)
            done(result);
    });
}

void FakeBackend::call(const Call& call, Done done)
{
    calls.push_back(call);
    if (responder) {
        if (std::optional<Result> result = responder(call)) {
            post(std::move(done), std::move(*result));
            return;
        }
    }
    m_pending.emplace_back(call, std::move(done));
}

void FakeBackend::reply(size_t index, Result result)
{
    Done done = std::move(m_pending.at(index).second);
    m_pending.erase(m_pending.begin() + ptrdiff_t(index));
    post(std::move(done), std::move(result));
}

void FakeBackend::replyAll(Result result)
{
    while (!m_pending.empty())
        reply(0, result);
}

void runPosted()
{
    EventLoop* loop = EventLoop::current();
    for (int round = 0; loop && round < 8; ++round)
        loop->dispatch();
}

Result error(const std::string& name, const std::string& message)
{
    return {name, message, {}};
}

std::string TreeBuilder::drivePath(const std::string& name)
{
    return std::string(kRoot) + "/drives/" + name;
}

std::string TreeBuilder::blockPath(const std::string& name)
{
    return std::string(kRoot) + "/block_devices/" + name;
}

static Variant path(const std::string& value)
{
    return Variant(ObjectPath{value.empty() ? std::string("/") : value});
}

static Variant bytes(const std::string& value)
{
    return Variant(encodeBytes(value));
}

std::string TreeBuilder::addDrive(const std::string& name, const DriveSpec& spec)
{
    const std::string object = drivePath(name);
    Model::Interfaces& interfaces = m_objects[object];
    interfaces[interface::kDrive] = {
        {"Vendor", Variant(spec.vendor)},
        {"Model", Variant(spec.model)},
        {"Serial", Variant(spec.serial)},
        {"Id", Variant(name)},
        {"Size", Variant(spec.size)},
        {"ConnectionBus", Variant(spec.connectionBus)},
        {"SortKey", Variant(spec.sortKey)},
        {"Removable", Variant(spec.removable)},
        {"MediaRemovable", Variant(spec.mediaRemovable)},
        {"MediaAvailable", Variant(true)},
        {"Ejectable", Variant(spec.ejectable)},
        {"CanPowerOff", Variant(spec.canPowerOff)},
        {"Optical", Variant(spec.optical)},
        {"Media", Variant(spec.media)},
        {"RotationRate", Variant(int32_t(spec.ata ? 5400 : 0))},
    };
    if (spec.nvme) {
        interfaces[interface::kNvmeController] = {
            {"State", Variant(std::string("live"))},
            {"SmartCriticalWarning", Variant(std::vector<std::string>())},
            {"SmartPowerOnHours", Variant(uint64_t(1108))},
            {"SmartTemperature", Variant(uint16_t(302))},
            {"SmartUpdated", Variant(uint64_t(1795176130))},
        };
    }
    if (spec.ata) {
        interfaces[interface::kAta] = {
            {"SmartSupported", Variant(true)},
            {"SmartEnabled", Variant(true)},
            {"SmartFailing", Variant(false)},
            {"SmartUpdated", Variant(uint64_t(1795176130))},
            {"SmartPowerOnSeconds", Variant(uint64_t(3600) * 4000)},
            {"SmartTemperature", Variant(310.15)},
            {"SmartNumBadSectors", Variant(int64_t(0))},
        };
    }
    return object;
}

static VariantMap blockProperties(const std::string& name, const std::string& drive,
                                  const TreeBuilder::BlockSpec& spec)
{
    return {
        {"Device", bytes("/dev/" + name)},
        {"PreferredDevice", bytes("/dev/" + name)},
        {"DeviceNumber", Variant(spec.deviceNumber)},
        {"Size", Variant(spec.size)},
        {"ReadOnly", Variant(spec.readOnly)},
        {"Drive", path(drive)},
        {"IdUsage", Variant(spec.idUsage)},
        {"IdType", Variant(spec.idType)},
        {"IdVersion", Variant(spec.idVersion)},
        {"IdLabel", Variant(spec.idLabel)},
        {"IdUUID", Variant(spec.idUuid)},
        {"CryptoBackingDevice", path({})},
        {"HintPartitionable", Variant(true)},
        {"HintSystem", Variant(spec.hintSystem)},
        {"HintIgnore", Variant(spec.hintIgnore)},
        {"HintName", Variant(std::string())},
        {"HintIconName", Variant(std::string())},
    };
}

std::string TreeBuilder::addDisk(const std::string& name, const std::string& drive, const BlockSpec& spec,
                                 const std::string& tableType)
{
    const std::string object = blockPath(name);
    Model::Interfaces& interfaces = m_objects[object];
    interfaces[interface::kBlock] = blockProperties(name, drive, spec);
    if (!tableType.empty())
        interfaces[interface::kPartitionTable] = {{"Type", Variant(tableType)},
                                                  {"Partitions", Variant(std::vector<ObjectPath>())}};
    return object;
}

std::string TreeBuilder::addPartition(const std::string& table, const std::string& name, uint32_t number,
                                      uint64_t offset, const BlockSpec& spec, const std::string& type,
                                      const std::string& partitionName, bool isContainer, bool isContained)
{
    const std::string object = blockPath(name);
    Model::Interfaces& tableInterfaces = m_objects[table];
    const std::string drive = tableInterfaces[interface::kBlock]["Drive"].value<ObjectPath>().path;
    Model::Interfaces& interfaces = m_objects[object];
    interfaces[interface::kBlock] = blockProperties(name, drive == "/" ? std::string() : drive, spec);
    interfaces[interface::kPartition] = {
        {"Number", Variant(number)},
        {"Type", Variant(type)},
        {"Flags", Variant(uint64_t(0))},
        {"Offset", Variant(offset)},
        {"Size", Variant(spec.size)},
        {"Name", Variant(partitionName)},
        {"UUID", Variant(name + "-uuid")},
        {"Table", path(table)},
        {"IsContainer", Variant(isContainer)},
        {"IsContained", Variant(isContained)},
    };
    VariantMap& partitionTable = tableInterfaces[interface::kPartitionTable];
    std::vector<ObjectPath> partitions = partitionTable["Partitions"].value<std::vector<ObjectPath>>();
    partitions.push_back({object});
    partitionTable["Partitions"] = Variant(partitions);
    return object;
}

std::string TreeBuilder::addCleartext(const std::string& name, const std::string& encrypted, const BlockSpec& spec)
{
    const std::string object = blockPath(name);
    Model::Interfaces& interfaces = m_objects[object];
    interfaces[interface::kBlock] = blockProperties(name, {}, spec);
    interfaces[interface::kBlock]["CryptoBackingDevice"] = path(encrypted);
    setEncrypted(encrypted);
    m_objects[encrypted][interface::kEncrypted]["CleartextDevice"] = path(object);
    return object;
}

std::string TreeBuilder::addJob(const std::string& name, const std::string& operation,
                                const std::vector<std::string>& objects, double progress, bool cancelable)
{
    const std::string object = std::string(kRoot) + "/jobs/" + name;
    std::vector<ObjectPath> paths;
    for (const std::string& item : objects)
        paths.push_back({item});
    m_objects[object][interface::kJob] = {
        {"Operation", Variant(operation)},
        {"Progress", Variant(progress)},
        {"ProgressValid", Variant(true)},
        {"Objects", Variant(paths)},
        {"Cancelable", Variant(cancelable)},
        {"StartedByUID", Variant(uint32_t(1000))},
    };
    return object;
}

void TreeBuilder::setFilesystem(const std::string& block, const std::vector<std::string>& mountPoints)
{
    std::vector<std::vector<uint8_t>> points;
    for (const std::string& point : mountPoints)
        points.push_back(encodeBytes(point));
    m_objects[block][interface::kFilesystem] = {{"MountPoints", Variant(points)}, {"Size", Variant(uint64_t(0))}};
}

void TreeBuilder::setEncrypted(const std::string& block)
{
    VariantMap& encrypted = m_objects[block][interface::kEncrypted];
    if (!encrypted.contains("CleartextDevice"))
        encrypted["CleartextDevice"] = path({});
    encrypted["HintEncryptionType"] = Variant(std::string("luks2"));
    encrypted["MetadataSize"] = Variant(uint64_t(16777216));
}

void TreeBuilder::setLoop(const std::string& block, const std::string& backingFile)
{
    m_objects[block][interface::kLoop] = {{"BackingFile", bytes(backingFile)},
                                          {"Autoclear", Variant(true)},
                                          {"SetupByUID", Variant(uint32_t(1000))}};
}

void TreeBuilder::setSwap(const std::string& block, bool active)
{
    m_objects[block][interface::kSwapspace] = {{"Active", Variant(active)}};
}

static constexpr uint64_t kMiB = 1024 * 1024;
static constexpr uint64_t kGB = 1000 * 1000 * 1000;

Model::Objects sampleTree()
{
    using DriveSpec = TreeBuilder::DriveSpec;
    using BlockSpec = TreeBuilder::BlockSpec;
    TreeBuilder tree;

    // The system disk.
    const uint64_t nvmeSize = 512110190592;
    const std::string nvmeDrive = tree.addDrive(
        "YMTC_PC005_512GB",
        DriveSpec{.model = "YMTC PC005 512GB", .serial = "YMA1512", .size = nvmeSize,
                  .sortKey = "00coldplug/00fixed/nvme0", .nvme = true});
    const std::string nvme = tree.addDisk("nvme0n1", nvmeDrive,
                                          BlockSpec{.size = nvmeSize, .deviceNumber = makedev(259, 0),
                                                    .hintSystem = true},
                                          "gpt");
    const std::string efi = tree.addPartition(
        nvme, "nvme0n1p1", 1, kMiB,
        BlockSpec{.size = 512 * kMiB, .deviceNumber = makedev(259, 1), .idUsage = "filesystem", .idType = "vfat",
                  .idVersion = "FAT32", .idLabel = "EFI", .idUuid = "46B8-D74C", .hintIgnore = true,
                  .hintSystem = true},
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "EFI");
    tree.setFilesystem(efi, {"/boot/efi"});
    const uint64_t rootOffset = kMiB + 512 * kMiB;
    const std::string root = tree.addPartition(
        nvme, "nvme0n1p2", 2, rootOffset,
        BlockSpec{.size = nvmeSize - rootOffset - kMiB, .deviceNumber = makedev(259, 2), .idUsage = "filesystem",
                  .idType = "ext4", .idVersion = "1.0", .idLabel = "ProperDesktop",
                  .idUuid = "1c8f081f-c3e2-4dc2-823a-fa455249cbb6", .hintSystem = true},
        "0fc63daf-8483-4772-8e79-3d69d8477de4");
    tree.setFilesystem(root, {"/"});

    // A second internal disk, half of it free.
    const uint64_t sataSize = 1000204886016;
    const std::string sataDrive = tree.addDrive(
        "ST1000LM035", DriveSpec{.vendor = "Seagate", .model = "ST1000LM035", .serial = "WL1", .size = sataSize,
                                 .sortKey = "00coldplug/00fixed/sda", .ata = true});
    const std::string sata = tree.addDisk(
        "sda", sataDrive, BlockSpec{.size = sataSize, .deviceNumber = makedev(8, 0), .hintSystem = true}, "gpt");
    const std::string data = tree.addPartition(
        sata, "sda1", 1, kMiB,
        BlockSpec{.size = 500 * kGB, .deviceNumber = makedev(8, 1), .idUsage = "filesystem", .idType = "ntfs",
                  .idLabel = "Data", .idUuid = "01D9", .hintSystem = true},
        "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Basic data partition");
    tree.setFilesystem(data, {});

    // A USB stick.
    const uint64_t stickSize = 16008609792;
    const std::string stickDrive = tree.addDrive(
        "SanDisk_Cruzer",
        DriveSpec{.vendor = "SanDisk", .model = "Cruzer Blade", .serial = "4C53", .size = stickSize,
                  .connectionBus = "usb", .sortKey = "01hotplug/1", .removable = true, .mediaRemovable = true,
                  .ejectable = true, .canPowerOff = true, .media = "thumb"});
    const std::string stick = tree.addDisk("sdb", stickDrive,
                                           BlockSpec{.size = stickSize, .deviceNumber = makedev(8, 16)}, "dos");
    const std::string stickVolume = tree.addPartition(
        stick, "sdb1", 1, kMiB,
        BlockSpec{.size = stickSize - kMiB, .deviceNumber = makedev(8, 17), .idUsage = "filesystem",
                  .idType = "vfat", .idVersion = "FAT32", .idLabel = "STICK", .idUuid = "A1B2-C3D4"},
        "0x0c");
    tree.setFilesystem(stickVolume, {"/media/u/STICK"});

    // A USB disk with encrypted volumes, an unformatted partition and room.
    const uint64_t usbSize = 500107862016;
    const std::string usbDrive = tree.addDrive(
        "WD_Elements", DriveSpec{.vendor = "WD", .model = "Elements 25A2", .serial = "WX1", .size = usbSize,
                                 .connectionBus = "usb", .sortKey = "01hotplug/2", .canPowerOff = true});
    const std::string usb = tree.addDisk("sdc", usbDrive,
                                         BlockSpec{.size = usbSize, .deviceNumber = makedev(8, 32)}, "gpt");
    const std::string unlocked = tree.addPartition(
        usb, "sdc1", 1, kMiB,
        BlockSpec{.size = 200 * kGB, .deviceNumber = makedev(8, 33), .idUsage = "crypto",
                  .idType = "crypto_LUKS", .idVersion = "2", .idUuid = "luks-1"},
        "0fc63daf-8483-4772-8e79-3d69d8477de4");
    const std::string cleartext = tree.addCleartext(
        "dm_2d0", unlocked,
        BlockSpec{.size = 200 * kGB - 16 * kMiB, .deviceNumber = makedev(254, 0), .idUsage = "filesystem",
                  .idType = "ext4", .idVersion = "1.0", .idLabel = "Secret", .idUuid = "secret-uuid"});
    tree.setFilesystem(cleartext, {"/media/u/Secret"});
    const std::string locked = tree.addPartition(
        usb, "sdc2", 2, kMiB + 200 * kGB,
        BlockSpec{.size = 100 * kGB, .deviceNumber = makedev(8, 34), .idUsage = "crypto",
                  .idType = "crypto_LUKS", .idVersion = "2", .idUuid = "luks-2"},
        "0fc63daf-8483-4772-8e79-3d69d8477de4");
    tree.setEncrypted(locked);
    tree.addPartition(usb, "sdc3", 3, kMiB + 300 * kGB, BlockSpec{.size = 50 * kGB, .deviceNumber = makedev(8, 35)},
                      "0fc63daf-8483-4772-8e79-3d69d8477de4");

    // An SD card with nothing on it.
    const uint64_t cardSize = 31914983424;
    const std::string cardDrive = tree.addDrive(
        "SD_Card", DriveSpec{.model = "SD Card", .size = cardSize, .connectionBus = "sdio",
                             .sortKey = "01hotplug/3", .removable = true, .mediaRemovable = true,
                             .ejectable = false, .media = "flash_sd"});
    tree.addDisk("sdd", cardDrive, BlockSpec{.size = cardSize, .deviceNumber = makedev(8, 48)});

    // A USB disk with an MBR extended partition.
    const uint64_t mbrSize = 320072933376;
    const std::string mbrDrive = tree.addDrive(
        "Toshiba_Canvio", DriveSpec{.vendor = "Toshiba", .model = "Canvio Basics", .serial = "T1", .size = mbrSize,
                                    .connectionBus = "usb", .sortKey = "01hotplug/4", .canPowerOff = true});
    const std::string mbr = tree.addDisk("sde", mbrDrive,
                                         BlockSpec{.size = mbrSize, .deviceNumber = makedev(8, 64)}, "dos");
    const std::string linuxVolume = tree.addPartition(
        mbr, "sde1", 1, kMiB,
        BlockSpec{.size = 100 * kGB, .deviceNumber = makedev(8, 65), .idUsage = "filesystem", .idType = "ext4",
                  .idLabel = "Linux", .idUuid = "linux-uuid"},
        "0x83");
    tree.setFilesystem(linuxVolume, {});
    const uint64_t extendedOffset = kMiB + 100 * kGB;
    tree.addPartition(mbr, "sde2", 2, extendedOffset, BlockSpec{.size = 200 * kGB, .deviceNumber = makedev(8, 66)},
                      "0x05", {}, true);
    const std::string shared = tree.addPartition(
        mbr, "sde5", 5, extendedOffset + kMiB,
        BlockSpec{.size = 150 * kGB, .deviceNumber = makedev(8, 69), .idUsage = "filesystem", .idType = "vfat",
                  .idVersion = "FAT32", .idLabel = "Shared", .idUuid = "5555-AAAA"},
        "0x0c", {}, false, true);
    tree.setFilesystem(shared, {});
    const std::string swap = tree.addPartition(
        mbr, "sde6", 6, extendedOffset + 2 * kMiB + 150 * kGB,
        BlockSpec{.size = 49 * kGB, .deviceNumber = makedev(8, 70), .idUsage = "other", .idType = "swap",
                  .idUuid = "swap-uuid"},
        "0x82", {}, false, true);
    tree.setSwap(swap, false);

    // A disk image, an unused loop device and compressed swap in memory.
    const std::string image = tree.addDisk(
        "loop0", {},
        BlockSpec{.size = 663748608, .deviceNumber = makedev(7, 0), .idUsage = "filesystem", .idType = "iso9660",
                  .idLabel = "Debian 13", .idUuid = "2026-11-01", .readOnly = true});
    tree.setLoop(image, "/home/u/Downloads/debian.iso");
    tree.setFilesystem(image, {"/media/u/Debian"});
    const std::string unused = tree.addDisk("loop1", {}, BlockSpec{.deviceNumber = makedev(7, 1)});
    tree.setLoop(unused, {});
    const std::string zram = tree.addDisk(
        "zram0", {},
        BlockSpec{.size = 4294967296, .deviceNumber = makedev(252, 0), .idUsage = "other", .idType = "swap",
                  .hintIgnore = true, .hintSystem = true});
    tree.setSwap(zram, true);

    return tree.objects();
}

SystemDevices sampleSystemDevices()
{
    return {{makedev(259, 1), makedev(259, 2), makedev(252, 0)}, true};
}

std::string addDataPartitionToSystemDisk(Model& model)
{
    TreeBuilder tree;
    tree.addDisk("nvme0n1", sample::kNvmeDrive,
                 TreeBuilder::BlockSpec{.size = 512110190592, .deviceNumber = makedev(259, 0)}, "gpt");
    const std::string windows = tree.addPartition(
        sample::kNvme, "nvme0n1p3", 3, 400 * kGB,
        TreeBuilder::BlockSpec{.size = 100 * kGB, .deviceNumber = makedev(259, 3), .idUsage = "filesystem",
                               .idType = "ntfs", .idLabel = "Windows", .idUuid = "WIN"},
        "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
    tree.setFilesystem(windows, {});
    model.addInterfaces(windows, tree.objects().at(windows));
    return windows;
}

void loadSample(Model& model)
{
    model.setSystemDevices(sampleSystemDevices());
    model.reset(sampleTree());
}

} // namespace udisks::fake
