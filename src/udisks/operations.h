// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/backend.h"

#include "base/signal.h"
#include "base/weak_ptr.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace udisks {

class Model;

// Block.Format's choices.
struct FormatOptions
{
    std::string label;
    // "zero" overwrites the device first; empty formats quickly.
    std::string erase;
    // Encrypts with LUKS when set.
    std::string passphrase;
    // "luks1" or "luks2"; empty takes UDisks2's default.
    std::string encryptionType;
    // The new filesystem's root belongs to the user (ext4, XFS, Btrfs).
    bool takeOwnership = true;
};

// Every operation the window performs, as asynchronous UDisks2 calls. Each
// takes the object it works on (a block, a drive, a job) by path and calls
// done once with the result, never from inside the call; a missing done is
// fine. Polkit asks for a password where UDisks2 wants one.
//
// The objects being worked on are busy until their call returns; a disk is
// busy while anything on it is. Calls that would destroy or take away the
// running system (format, delete, resize, create, repair or restore onto a
// block that holds it, unmounting a system mount point) fail at once with
// kSystemError and are never sent.
class Operations
{
public:
    static constexpr const char* kSystemError = "org.properdesktop.DiskUtility.Error.System";
    static constexpr const char* kBusyError = "org.properdesktop.DiskUtility.Error.Busy";

    Operations(Model& model, Backend& backend);
    ~Operations();

    Operations(const Operations&) = delete;
    Operations& operator=(const Operations&) = delete;

    // Filesystem. For an encrypted volume these take its cleartext block.
    // Mount's result string() is the mount point.
    void mount(const std::string& block, Done done = {});
    void unmount(const std::string& block, Done done = {});
    void setLabel(const std::string& block, const std::string& label, Done done = {});
    // boolean(): the filesystem is consistent, or was repaired. Unmounted
    // filesystems only.
    void check(const std::string& block, Done done = {});
    void repair(const std::string& block, Done done = {});
    void resizeFilesystem(const std::string& block, uint64_t size, Done done = {});

    // Block. type is a filesystem ("ext4", "vfat", "exfat", "ntfs", "btrfs",
    // "xfs"), a partition table ("gpt", "dos") or "empty".
    void format(const std::string& block, const std::string& type, const FormatOptions& options, Done done = {});
    // The values hold a UnixFd open on the device.
    void openForBackup(const std::string& block, Done done = {});
    void openForRestore(const std::string& block, Done done = {});

    // Partition table and partitions. objectPath() is the new partition.
    void createPartition(const std::string& table, uint64_t offset, uint64_t size, const std::string& type,
                         const std::string& name, Done done = {});
    void createPartitionAndFormat(const std::string& table, uint64_t offset, uint64_t size,
                                  const std::string& partitionType, const std::string& name,
                                  const std::string& filesystem, const FormatOptions& options, Done done = {});
    void deletePartition(const std::string& block, Done done = {});
    void resizePartition(const std::string& block, uint64_t size, Done done = {});
    void setPartitionName(const std::string& block, const std::string& name, Done done = {});
    void setPartitionType(const std::string& block, const std::string& type, Done done = {});

    // Encryption. Unlock's objectPath() is the cleartext block.
    void unlock(const std::string& block, const std::string& passphrase, Done done = {});
    void lock(const std::string& block, Done done = {});
    void changePassphrase(const std::string& block, const std::string& passphrase, const std::string& newPassphrase,
                          Done done = {});

    // The whole disk's block: unmounts every filesystem on it and locks its
    // encrypted volumes, then ejects the drive, or powers it off when it
    // cannot be ejected; a disk image is detached instead.
    void ejectDisk(const std::string& disk, Done done = {});

    // Disk images. LoopSetup's objectPath() is the loop device.
    void loopSetup(const std::string& file, bool readOnly, Done done = {});
    void loopDelete(const std::string& block, Done done = {});

    // S.M.A.R.T. of the drive: Drive.Ata or NVMe.Controller, whichever it has.
    void smartUpdate(const std::string& drive, Done done = {});
    // Drive.Ata: a(ysqiiixia{sv}); NVMe: a{sv}.
    void smartGetAttributes(const std::string& drive, Done done = {});
    // "short", "extended" or "conveyance" (ATA only).
    void smartSelftestStart(const std::string& drive, const std::string& type, Done done = {});

    void cancelJob(const std::string& job, Done done = {});

    // Manager: whether the tools for the filesystem are installed, and the
    // missing tool's name when not.
    using Availability = std::function<void(bool available, const std::string& missing)>;
    void canFormat(const std::string& type, Availability done);
    void canCheck(const std::string& type, Availability done);
    void canRepair(const std::string& type, Availability done);
    // modes: libblockdev's resize flags, which UDisks2 passes on (2 offline
    // shrink, 4 offline grow, 8 online shrink, 16 online grow; ext4 is 22).
    void canResize(const std::string& type, std::function<void(bool, uint64_t modes, const std::string&)> done);

    // The object, or with drives and disks anything on them, has a call out.
    bool isBusy(const std::string& path) const;
    // Any call is out or any hold kept.
    bool isAnyBusy() const { return !m_running.empty(); }

    // Keeps an object busy, as a call out on it does, for as long as it
    // lives: for work on a device that goes past UDisks2, such as a copy.
    // Calls on it, the holder's own included, are refused meanwhile.
    class Hold
    {
    public:
        Hold() = default;
        ~Hold() { reset(); }
        Hold(Hold&& other) noexcept;
        Hold& operator=(Hold&& other) noexcept;

        void reset();

    private:
        friend class Operations;
        Hold(properdesktop::base::WeakPtr<Operations> operations, uint64_t id);

        properdesktop::base::WeakPtr<Operations> m_operations;
        uint64_t m_id = 0;
    };
    [[nodiscard]] Hold hold(const std::string& path);

    properdesktop::base::Signal<> busyChanged;
    // Every result, after its done: the call's method and object, as for a
    // window's error alert.
    properdesktop::base::Signal<const Call&, const Result&> finished;

private:
    struct Step
    {
        Call call;
    };

    void run(const std::string& subject, std::vector<Step> steps, Done done);
    void runStep(uint64_t id, std::vector<Step> steps, size_t index, Done done);
    void refuse(Call call, const char* error, const std::string& message, Done done);
    // The whole disk a path belongs to; itself when it is none.
    std::string diskOf(const std::string& path) const;

    Model& m_model;
    Backend& m_backend;
    uint64_t m_nextId = 1;
    // A running call's or a hold's subject and its disk.
    std::map<uint64_t, std::pair<std::string, std::string>> m_running;
    properdesktop::base::WeakPtrFactory<Operations> m_weakFactory{this};
};

} // namespace udisks
