// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/backend.h"
#include "udisks/model.h"
#include "udisks/system.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// UDisks2 without a bus, for the tests: an object tree built in code, fed to
// a Model with reset(), and a backend that records the calls and answers
// them when the test says.
namespace udisks::fake {

// Records every call. A call is answered by the responder when it gives a
// result, otherwise it waits for reply(); either way done runs from the
// event loop, as on the bus.
class FakeBackend : public Backend
{
public:
    void call(const Call& call, Done done) override;

    // Every call made, in order.
    std::vector<Call> calls;
    // Answers at once when it returns a result.
    std::function<std::optional<Result>(const Call&)> responder;

    // The calls still waiting for an answer, oldest first.
    size_t pendingCount() const { return m_pending.size(); }
    const Call& pending(size_t index) const { return m_pending.at(index).first; }
    // Answers a waiting call; done runs on the next loop turn.
    void reply(size_t index, Result result = {});
    void replyAll(Result result = {});

private:
    std::vector<std::pair<Call, Done>> m_pending;
};

// Runs what is posted to the thread's event loop, and what that posts in
// turn, a few rounds deep: the answers of the fake backend and the
// operations.
void runPosted();

// An error result, as UDisks2 sends.
Result error(const std::string& name, const std::string& message = "failed");

// Builds GetManagedObjects' answer. Paths are UDisks2's:
// /org/freedesktop/UDisks2/drives/<name> and .../block_devices/<name>.
class TreeBuilder
{
public:
    struct DriveSpec
    {
        std::string vendor;
        std::string model;
        std::string serial;
        uint64_t size = 0;
        std::string connectionBus;
        std::string sortKey;
        bool removable = false;
        bool mediaRemovable = false;
        bool ejectable = false;
        bool canPowerOff = false;
        bool optical = false;
        std::string media;
        // Adds NVMe.Controller or Drive.Ata.
        bool nvme = false;
        bool ata = false;
    };

    struct BlockSpec
    {
        uint64_t size = 0;
        // makedev(major, minor).
        uint64_t deviceNumber = 0;
        std::string idUsage;
        std::string idType;
        std::string idVersion;
        std::string idLabel;
        std::string idUuid;
        bool readOnly = false;
        bool hintIgnore = false;
        bool hintSystem = false;
    };

    static std::string drivePath(const std::string& name);
    static std::string blockPath(const std::string& name);

    std::string addDrive(const std::string& name, const DriveSpec& spec);
    // A whole disk of the drive (empty for a loop device), with a partition
    // table when tableType ("gpt", "dos") is set.
    std::string addDisk(const std::string& name, const std::string& drive, const BlockSpec& spec,
                        const std::string& tableType = {});
    std::string addPartition(const std::string& table, const std::string& name, uint32_t number, uint64_t offset,
                             const BlockSpec& spec, const std::string& type = {}, const std::string& partitionName = {},
                             bool isContainer = false, bool isContained = false);
    // A cleartext block unlocked from an encrypted one; links both ways.
    std::string addCleartext(const std::string& name, const std::string& encrypted, const BlockSpec& spec);
    std::string addJob(const std::string& name, const std::string& operation,
                       const std::vector<std::string>& objects, double progress = 0, bool cancelable = true);

    void setFilesystem(const std::string& block, const std::vector<std::string>& mountPoints);
    void setEncrypted(const std::string& block);
    void setLoop(const std::string& block, const std::string& backingFile);
    void setSwap(const std::string& block, bool active);

    // The interfaces of an object, to change before or after reset().
    Model::Interfaces& interfaces(const std::string& path) { return m_objects[path]; }
    const Model::Objects& objects() const { return m_objects; }

private:
    Model::Objects m_objects;
};

// A laptop's disks, attached things and images:
//
//   nvme0n1   internal NVMe, GPT: p1 EFI (vfat, /boot/efi, HintIgnore),
//             p2 "ProperDesktop" (ext4, /)
//   sda       internal SATA, GPT: sda1 "Data" (NTFS, unmounted), free space
//   sdb       USB stick, ejectable, MBR: sdb1 "STICK" (FAT32, /media/u/STICK)
//   sdc       USB disk, power off, GPT: sdc1 LUKS unlocked as dm-0 "Secret"
//             (ext4, /media/u/Secret), sdc2 LUKS locked, sdc3 unformatted
//   sdd       SD card, no table and no filesystem
//   sde       USB disk, MBR: sde1 "Linux" (ext4), sde2 extended holding
//             sde5 "Shared" (FAT32) and sde6 swap (inactive)
//   loop0     debian.iso, read-only, iso9660 "Debian 13" (/media/u/Debian)
//   loop1     unused; zram0 swap in use without a drive
Model::Objects sampleTree();
// nvme0n1p1, nvme0n1p2 and zram0, as probeSystemDevices() finds them there.
SystemDevices sampleSystemDevices();
// A model filled with both.
void loadSample(Model& model);
// Adds nvme0n1p3, an unmounted NTFS "Windows" partition, to the sample's
// system disk: a volume that does not hold the system on a disk that does.
std::string addDataPartitionToSystemDisk(Model& model);

namespace sample {
inline const std::string kNvmeDrive = TreeBuilder::drivePath("YMTC_PC005_512GB");
inline const std::string kNvme = TreeBuilder::blockPath("nvme0n1");
inline const std::string kEfi = TreeBuilder::blockPath("nvme0n1p1");
inline const std::string kRoot = TreeBuilder::blockPath("nvme0n1p2");
inline const std::string kSataDrive = TreeBuilder::drivePath("ST1000LM035");
inline const std::string kSata = TreeBuilder::blockPath("sda");
inline const std::string kData = TreeBuilder::blockPath("sda1");
inline const std::string kStickDrive = TreeBuilder::drivePath("SanDisk_Cruzer");
inline const std::string kStick = TreeBuilder::blockPath("sdb");
inline const std::string kStickVolume = TreeBuilder::blockPath("sdb1");
inline const std::string kUsbDrive = TreeBuilder::drivePath("WD_Elements");
inline const std::string kUsb = TreeBuilder::blockPath("sdc");
inline const std::string kUnlocked = TreeBuilder::blockPath("sdc1");
inline const std::string kCleartext = TreeBuilder::blockPath("dm_2d0");
inline const std::string kLocked = TreeBuilder::blockPath("sdc2");
inline const std::string kUnformatted = TreeBuilder::blockPath("sdc3");
inline const std::string kCardDrive = TreeBuilder::drivePath("SD_Card");
inline const std::string kCard = TreeBuilder::blockPath("sdd");
inline const std::string kMbrDrive = TreeBuilder::drivePath("Toshiba_Canvio");
inline const std::string kMbr = TreeBuilder::blockPath("sde");
inline const std::string kMbrLinux = TreeBuilder::blockPath("sde1");
inline const std::string kExtended = TreeBuilder::blockPath("sde2");
inline const std::string kLogicalFat = TreeBuilder::blockPath("sde5");
inline const std::string kLogicalSwap = TreeBuilder::blockPath("sde6");
inline const std::string kImage = TreeBuilder::blockPath("loop0");
inline const std::string kUnusedLoop = TreeBuilder::blockPath("loop1");
inline const std::string kZram = TreeBuilder::blockPath("zram0");
} // namespace sample

} // namespace udisks::fake
