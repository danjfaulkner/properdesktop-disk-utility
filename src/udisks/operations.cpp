// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/operations.h"
#include "udisks/guard.h"
#include "udisks/model.h"

#include "base/event_loop.h"
#include "base/i18n.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <tuple>
#include <utility>

#include <fcntl.h>

using properdesktop::base::EventLoop;
using properdesktop::base::dbus::ObjectPath;
using properdesktop::base::dbus::UnixFd;
using properdesktop::i18n::tr;
using namespace std::chrono_literals;

namespace udisks {

// Long enough for mkfs, fsck or a copy of a large disk; their progress is
// followed through the job.
static constexpr std::chrono::seconds kLong = 24h;

bool Result::cancelled() const
{
    return errorName == "org.freedesktop.UDisks2.Error.Cancelled"
            || errorName == "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed";
}

std::string Result::string() const
{
    return values.empty() ? std::string() : values.front().value<std::string>();
}

std::string Result::objectPath() const
{
    return values.empty() ? std::string() : values.front().value<ObjectPath>().path;
}

bool Result::boolean() const
{
    return !values.empty() && values.front().value<bool>();
}

// Calls that may ask for a password wait as long as the user takes: a
// timeout would report a failure for what UDisks2 then does.
static Call makeCall(const std::string& path, const char* interface, const char* method, Variant::Items arguments,
                     std::chrono::seconds timeout = kLong)
{
    return {path, interface, method, std::move(arguments), timeout};
}

static Variant options(VariantMap map = {})
{
    return Variant(map);
}

static VariantMap formatOptions(const FormatOptions& options, bool retype = true, bool tearDown = false)
{
    VariantMap map;
    if (!options.label.empty())
        map["label"] = Variant(options.label);
    if (!options.erase.empty())
        map["erase"] = Variant(options.erase);
    if (!options.passphrase.empty()) {
        map["encrypt.passphrase"] = Variant(options.passphrase);
        if (!options.encryptionType.empty())
            map["encrypt.type"] = Variant(options.encryptionType);
    }
    map["take-ownership"] = Variant(options.takeOwnership);
    // Tear-down also removes the block's /etc/fstab and /etc/crypttab entries,
    // and UDisks2 asks for the password again on every call that sets it.
    // The plans unmount and lock first, so it is set only when there are
    // entries to remove.
    map["tear-down"] = Variant(tearDown);
    // Sets the partition's type to match the filesystem unless the
    // partition table may not change.
    map["update-partition-type"] = Variant(retype);
    return map;
}

// UDisks2's tear-down walks the same blocks: the partitions of a table, and
// the cleartext block of an unlocked one or the child entries of a locked one.
static bool hasConfiguration(const Model& model, const std::string& path)
{
    const Object* object = model.object(path);
    if (!object || !object->block)
        return false;
    if (object->block->configured)
        return true;
    if (object->partitionTable) {
        for (const Object* partition : model.partitionsOf(path)) {
            if (hasConfiguration(model, partition->path))
                return true;
        }
    }
    if (object->encrypted) {
        if (const Object* cleartext = model.cleartextOf(path))
            return hasConfiguration(model, cleartext->path);
        return object->encrypted->childConfigured;
    }
    return false;
}

Operations::Operations(Model& model, Backend& backend)
    : m_model(model)
    , m_backend(backend)
{
}

Operations::~Operations() = default;

std::string Operations::diskOf(const std::string& path) const
{
    const Object* object = m_model.object(path);
    if (!object)
        return path;
    if (object->drive) {
        const Object* whole = m_model.wholeBlock(path);
        return whole ? whole->path : path;
    }
    if (!object->block)
        return path;
    if (!object->block->cryptoBackingDevice.empty())
        return diskOf(object->block->cryptoBackingDevice);
    if (object->partition)
        return object->partition->table;
    return path;
}

bool Operations::isBusy(const std::string& path) const
{
    const std::string disk = diskOf(path);
    const bool isDisk = disk == path || (m_model.object(path) && m_model.object(path)->drive);
    for (const auto& [id, running] : m_running) {
        if (running.first == path || running.first == disk)
            return true;
        if (isDisk && running.second == disk)
            return true;
    }
    return false;
}

Operations::Hold::Hold(properdesktop::base::WeakPtr<Operations> operations, uint64_t id)
    : m_operations(std::move(operations))
    , m_id(id)
{
}

Operations::Hold::Hold(Hold&& other) noexcept
    : m_operations(std::move(other.m_operations))
    , m_id(std::exchange(other.m_id, 0))
{
}

Operations::Hold& Operations::Hold::operator=(Hold&& other) noexcept
{
    if (this != &other) {
        reset();
        m_operations = std::move(other.m_operations);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

void Operations::Hold::reset()
{
    if (Operations* operations = m_operations.get(); operations && m_id) {
        operations->m_running.erase(m_id);
        operations->busyChanged.emit();
    }
    m_id = 0;
}

Operations::Hold Operations::hold(const std::string& path)
{
    const uint64_t id = m_nextId++;
    m_running[id] = {path, diskOf(path)};
    busyChanged.emit();
    return Hold(m_weakFactory.getWeakPtr(), id);
}

void Operations::refuse(Call call, const char* error, const std::string& message, Done done)
{
    EventLoop::current()->post(
        [self = m_weakFactory.getWeakPtr(), call = std::move(call), error, message, done = std::move(done)] {
            const Result result{error, message, {}};
            if (done)
                done(result);
            if (Operations* operations = self.get())
                operations->finished.emit(call, result);
        });
}

void Operations::run(const std::string& subject, std::vector<Step> steps, Done done)
{
    if (steps.empty())
        return;
    if (isBusy(subject)) {
        refuse(steps.front().call, kBusyError, tr("Another operation on this disk is in progress."), std::move(done));
        return;
    }
    const uint64_t id = m_nextId++;
    m_running[id] = {subject, diskOf(subject)};
    busyChanged.emit();
    runStep(id, std::move(steps), 0, std::move(done));
}

void Operations::runStep(uint64_t id, std::vector<Step> steps, size_t index, Done done)
{
    const Call call = steps[index].call;
    m_backend.call(call, [self = m_weakFactory.getWeakPtr(), id, steps = std::move(steps), index,
                          done = std::move(done), call](const Result& result) mutable {
        Operations* operations = self.get();
        if (!operations)
            return;
        if (result.ok() && index + 1 < steps.size()) {
            operations->runStep(id, std::move(steps), index + 1, std::move(done));
            return;
        }
        operations->m_running.erase(id);
        operations->busyChanged.emit();
        if (done)
            done(result);
        if (Operations* still = self.get())
            still->finished.emit(call, result);
    });
}

void Operations::mount(const std::string& block, Done done)
{
    run(block, {{makeCall(block, interface::kFilesystem, "Mount", {options()})}}, std::move(done));
}

void Operations::unmount(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kFilesystem, "Unmount", {options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Volume); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::setLabel(const std::string& block, const std::string& label, Done done)
{
    Call call = makeCall(block, interface::kFilesystem, "SetLabel", {Variant(label), options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Volume); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::check(const std::string& block, Done done)
{
    run(block, {{makeCall(block, interface::kFilesystem, "Check", {options()}, kLong)}}, std::move(done));
}

void Operations::repair(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kFilesystem, "Repair", {options()}, kLong);
    if (const std::string why = systemRefusal(m_model, block, Change::Volume); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::resizeFilesystem(const std::string& block, uint64_t size, Done done)
{
    Call call = makeCall(block, interface::kFilesystem, "Resize", {Variant(size), options()}, kLong);
    if (const std::string why = systemRefusal(m_model, block, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::format(const std::string& block, const std::string& type, const FormatOptions& formatting,
                        Done done)
{
    // On the system's disk a partition is formatted in place, its table as it
    // is: a new type would be a change to the running disk's table.
    const bool retype = systemRefusal(m_model, block, Change::Table).empty();
    Call call = makeCall(block, interface::kBlock, "Format",
                         {Variant(type), options(formatOptions(formatting, retype, hasConfiguration(m_model, block)))},
                         kLong);
    if (const std::string why = systemRefusal(m_model, block, formatChange(m_model, block)); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::openForBackup(const std::string& block, Done done)
{
    run(block, {{makeCall(block, interface::kBlock, "OpenForBackup", {options()})}}, std::move(done));
}

void Operations::openForRestore(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kBlock, "OpenForRestore", {options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::createPartition(const std::string& table, uint64_t offset, uint64_t size, const std::string& type,
                                 const std::string& name, Done done)
{
    Call call = makeCall(table, interface::kPartitionTable, "CreatePartition",
                         {Variant(offset), Variant(size), Variant(type), Variant(name), options()});
    if (const std::string why = systemRefusal(m_model, table, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(table, {{std::move(call)}}, std::move(done));
}

void Operations::createPartitionAndFormat(const std::string& table, uint64_t offset, uint64_t size,
                                          const std::string& partitionType, const std::string& name,
                                          const std::string& filesystem, const FormatOptions& formatting, Done done)
{
    Call call = makeCall(table, interface::kPartitionTable, "CreatePartitionAndFormat",
                         {Variant(offset), Variant(size), Variant(partitionType), Variant(name), options(),
                          Variant(filesystem), options(formatOptions(formatting))},
                         kLong);
    if (const std::string why = systemRefusal(m_model, table, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(table, {{std::move(call)}}, std::move(done));
}

void Operations::deletePartition(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kPartition, "Delete", {options({{"tear-down", Variant(true)}})});
    if (const std::string why = systemRefusal(m_model, block, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::resizePartition(const std::string& block, uint64_t size, Done done)
{
    Call call = makeCall(block, interface::kPartition, "Resize", {Variant(size), options()}, kLong);
    if (const std::string why = systemRefusal(m_model, block, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::setPartitionName(const std::string& block, const std::string& name, Done done)
{
    run(block, {{makeCall(block, interface::kPartition, "SetName", {Variant(name), options()})}}, std::move(done));
}

void Operations::setPartitionType(const std::string& block, const std::string& type, Done done)
{
    Call call = makeCall(block, interface::kPartition, "SetType", {Variant(type), options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Table); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::unlock(const std::string& block, const std::string& passphrase, Done done)
{
    run(block, {{makeCall(block, interface::kEncrypted, "Unlock", {Variant(passphrase), options()})}},
        std::move(done));
}

void Operations::lock(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kEncrypted, "Lock", {options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Volume); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

void Operations::changePassphrase(const std::string& block, const std::string& passphrase,
                                  const std::string& newPassphrase, Done done)
{
    run(block,
        {{makeCall(block, interface::kEncrypted, "ChangePassphrase",
                   {Variant(passphrase), Variant(newPassphrase), options()})}},
        std::move(done));
}

void Operations::ejectDisk(const std::string& disk, Done done)
{
    const Object* whole = m_model.object(disk);
    std::vector<Step> steps;
    if (!whole || !whole->block) {
        refuse(makeCall(disk, interface::kBlock, "Eject", {}), "org.freedesktop.UDisks2.Error.NotFound",
               tr("The disk is no longer available."), std::move(done));
        return;
    }
    std::vector<const Object*> blocks;
    if (!whole->block->drive.empty()) {
        blocks = m_model.blocksOfDrive(whole->block->drive);
    } else {
        blocks.push_back(whole);
        for (const Object* partition : m_model.partitionsOf(disk))
            blocks.push_back(partition);
    }
    // Cleartext blocks first, so each is unmounted before its encrypted
    // block is locked.
    std::vector<const Object*> ordered;
    for (const Object* block : blocks) {
        if (const Object* clear = m_model.cleartextOf(block->path))
            ordered.push_back(clear);
    }
    for (const Object* block : blocks) {
        if (std::find(ordered.begin(), ordered.end(), block) == ordered.end())
            ordered.push_back(block);
    }
    for (const Object* block : ordered) {
        if (block->filesystem && !block->filesystem->mountPoints.empty())
            steps.push_back({makeCall(block->path, interface::kFilesystem, "Unmount", {options()})});
        if (block->encrypted && m_model.cleartextOf(block->path))
            steps.push_back({makeCall(block->path, interface::kEncrypted, "Lock", {options()})});
    }

    Call last;
    if (whole->loop) {
        last = makeCall(disk, interface::kLoop, "Delete", {options()});
    } else {
        const Object* drive = m_model.object(whole->block->drive);
        const bool ejectable = drive && drive->drive && drive->drive->ejectable;
        const bool canPowerOff = drive && drive->drive && drive->drive->canPowerOff;
        if (!ejectable && !canPowerOff) {
            refuse(makeCall(whole->block->drive, interface::kDrive, "Eject", {options()}),
                   "org.freedesktop.UDisks2.Error.NotSupported", tr("The disk cannot be ejected."),
                   std::move(done));
            return;
        }
        last = makeCall(whole->block->drive, interface::kDrive, ejectable ? "Eject" : "PowerOff", {options()});
    }
    if (const std::string why = systemRefusal(m_model, disk, Change::Drive); !why.empty()) {
        refuse(std::move(last), kSystemError, why, std::move(done));
        return;
    }
    steps.push_back({std::move(last)});
    run(disk, std::move(steps), std::move(done));
}

void Operations::loopSetup(const std::string& file, bool readOnly, Done done)
{
    const int fd = ::open(file.c_str(), (readOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    Call call = makeCall(kManagerPath, interface::kManager, "LoopSetup", {});
    if (fd < 0) {
        refuse(std::move(call), "org.properdesktop.DiskUtility.Error.File", std::strerror(errno), std::move(done));
        return;
    }
    call.arguments = {Variant(UnixFd::adopt(fd)), options({{"read-only", Variant(readOnly)}})};
    run(file, {{std::move(call)}}, std::move(done));
}

void Operations::loopDelete(const std::string& block, Done done)
{
    Call call = makeCall(block, interface::kLoop, "Delete", {options()});
    if (const std::string why = systemRefusal(m_model, block, Change::Drive); !why.empty()) {
        refuse(std::move(call), kSystemError, why, std::move(done));
        return;
    }
    run(block, {{std::move(call)}}, std::move(done));
}

static const char* smartInterface(const Model& model, const std::string& drive)
{
    const Object* object = model.object(drive);
    return object && object->nvme ? interface::kNvmeController : interface::kAta;
}

void Operations::smartUpdate(const std::string& drive, Done done)
{
    run(drive, {{makeCall(drive, smartInterface(m_model, drive), "SmartUpdate", {options()})}}, std::move(done));
}

void Operations::smartGetAttributes(const std::string& drive, Done done)
{
    // Reading leaves the drive free for anything else.
    m_backend.call(makeCall(drive, smartInterface(m_model, drive), "SmartGetAttributes", {options()}),
                   std::move(done));
}

void Operations::smartSelftestStart(const std::string& drive, const std::string& type, Done done)
{
    m_backend.call(makeCall(drive, smartInterface(m_model, drive), "SmartSelftestStart", {Variant(type), options()}),
                   std::move(done));
}

void Operations::cancelJob(const std::string& job, Done done)
{
    m_backend.call(makeCall(job, interface::kJob, "Cancel", {options()}), std::move(done));
}

static void askAvailability(Backend& backend, const char* method, const std::string& type,
                            Operations::Availability done)
{
    backend.call(makeCall(kManagerPath, interface::kManager, method, {Variant(type)}, 30s),
                 [done = std::move(done)](const Result& result) {
                     if (!done)
                         return;
                     const auto answer = result.values.empty()
                             ? std::nullopt
                             : result.values.front().get<std::tuple<bool, std::string>>();
                     if (answer)
                         done(std::get<0>(*answer), std::get<1>(*answer));
                     else
                         done(false, {});
                 });
}

void Operations::canFormat(const std::string& type, Availability done)
{
    askAvailability(m_backend, "CanFormat", type, std::move(done));
}

void Operations::canCheck(const std::string& type, Availability done)
{
    askAvailability(m_backend, "CanCheck", type, std::move(done));
}

void Operations::canRepair(const std::string& type, Availability done)
{
    askAvailability(m_backend, "CanRepair", type, std::move(done));
}

void Operations::canResize(const std::string& type, std::function<void(bool, uint64_t, const std::string&)> done)
{
    m_backend.call(makeCall(kManagerPath, interface::kManager, "CanResize", {Variant(type)}, 30s),
                   [done = std::move(done)](const Result& result) {
                       if (!done)
                           return;
                       const auto answer = result.values.empty()
                               ? std::nullopt
                               : result.values.front().get<std::tuple<bool, uint64_t, std::string>>();
                       if (answer)
                           done(std::get<0>(*answer), std::get<1>(*answer), std::get<2>(*answer));
                       else
                           done(false, 0, {});
                   });
}

} // namespace udisks
