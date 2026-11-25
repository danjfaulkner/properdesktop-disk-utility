// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>
#include <vector>

#include <unistd.h>

using namespace udisks;
using namespace udisks::fake;
using properdesktop::base::dbus::ObjectPath;
using properdesktop::base::dbus::UnixFd;

static Result value(Variant item)
{
    return {{}, {}, {std::move(item)}};
}

class OperationsTest : public testing::Test
{
protected:
    void SetUp() override { loadSample(model); }

    // The a{sv} argument at the index.
    VariantMap options(const Call& call, size_t index)
    {
        return call.arguments.at(index).value<VariantMap>();
    }

    properdesktop::base::EventLoop loop;
    Model model;
    FakeBackend backend;
    Operations operations{model, backend};
};

TEST_F(OperationsTest, Mount)
{
    std::optional<Result> result;
    std::vector<std::string> finished;
    auto subscription = operations.finished.connect(
        [&](const Call& call, const Result&) { finished.push_back(call.method); });
    operations.mount(sample::kData, [&](const Result& r) { result = r; });

    ASSERT_EQ(backend.calls.size(), 1u);
    const Call& call = backend.calls.front();
    EXPECT_EQ(call.path, sample::kData);
    EXPECT_EQ(call.interface, interface::kFilesystem);
    EXPECT_EQ(call.method, "Mount");
    ASSERT_EQ(call.arguments.size(), 1u);
    EXPECT_EQ(call.arguments[0].signature(), "a{sv}");
    EXPECT_TRUE(operations.isBusy(sample::kData));
    EXPECT_TRUE(operations.isBusy(sample::kSata));
    EXPECT_TRUE(operations.isBusy(sample::kSataDrive));
    EXPECT_FALSE(operations.isBusy(sample::kRoot));

    backend.reply(0, value(Variant(std::string("/media/u/Data"))));
    EXPECT_FALSE(result);
    runPosted();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->ok());
    EXPECT_EQ(result->string(), "/media/u/Data");
    EXPECT_EQ(finished, std::vector<std::string>{"Mount"});
    EXPECT_FALSE(operations.isBusy(sample::kData));
}

TEST_F(OperationsTest, OneCallAtATimePerObject)
{
    std::optional<Result> second;
    operations.mount(sample::kData);
    operations.mount(sample::kData, [&](const Result& r) { second = r; });
    EXPECT_EQ(backend.calls.size(), 1u);
    runPosted();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->errorName, Operations::kBusyError);

    // Formatting the disk waits for its volume too.
    std::optional<Result> format;
    operations.format(sample::kSata, "gpt", {}, [&](const Result& r) { format = r; });
    runPosted();
    ASSERT_TRUE(format);
    EXPECT_EQ(format->errorName, Operations::kBusyError);

    // Another disk is free.
    operations.unmount(sample::kStickVolume);
    EXPECT_EQ(backend.calls.size(), 2u);
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, TheSystemIsNeverTouched)
{
    std::vector<std::string> errors;
    const auto collect = [&](const Result& r) { errors.push_back(r.errorName); };
    operations.unmount(sample::kRoot, collect);
    operations.unmount(sample::kEfi, collect);
    operations.format(sample::kRoot, "ext4", {}, collect);
    operations.format(sample::kNvme, "gpt", {}, collect);
    operations.deletePartition(sample::kEfi, collect);
    operations.resizePartition(sample::kRoot, 1, collect);
    operations.resizeFilesystem(sample::kRoot, 1, collect);
    operations.createPartition(sample::kNvme, 0, 1, {}, {}, collect);
    operations.createPartitionAndFormat(sample::kNvme, 0, 1, {}, {}, "ext4", {}, collect);
    operations.repair(sample::kRoot, collect);
    operations.openForRestore(sample::kNvme, collect);
    operations.setPartitionType(sample::kRoot, "0x83", collect);
    operations.ejectDisk(sample::kNvme, collect);
    EXPECT_TRUE(backend.calls.empty());
    runPosted();
    ASSERT_EQ(errors.size(), 13u);
    for (size_t i = 0; i < errors.size(); ++i) {
        // The NVMe disk can neither be ejected nor powered off, which is said first.
        if (i == 12)
            EXPECT_EQ(errors[i], "org.freedesktop.UDisks2.Error.NotSupported");
        else
            EXPECT_EQ(errors[i], Operations::kSystemError) << i;
    }

    // A swap partition in use, and an encrypted volume holding /home.
    model.changeProperties(sample::kLogicalSwap, interface::kSwapspace, {{"Active", Variant(true)}});
    model.changeProperties(sample::kCleartext, interface::kFilesystem,
                           {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>{encodeBytes("/home")})}});
    operations.format(sample::kMbr, "gpt", {});
    operations.deletePartition(sample::kExtended);
    operations.lock(sample::kUnlocked);
    operations.format(sample::kUnlocked, "ext4", {});
    operations.ejectDisk(sample::kUsb);
    EXPECT_TRUE(backend.calls.empty());
    runPosted();

    // Reading the system disk is fine.
    operations.openForBackup(sample::kNvme);
    EXPECT_EQ(backend.calls.size(), 1u);
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, GuardsFailClosed)
{
    std::vector<Result> results;
    const auto collect = [&](const Result& r) { results.push_back(r); };
    const std::string gone = TreeBuilder::blockPath("sdq1");
    operations.format(gone, "ext4", {}, collect);
    operations.deletePartition(gone, collect);
    operations.setLabel(sample::kRoot, "Main", collect);
    // Nothing is known of the system: nothing changes.
    model.setSystemDevices({});
    operations.format(sample::kStickVolume, "vfat", {}, collect);
    operations.unmount(sample::kStickVolume, collect);
    EXPECT_TRUE(backend.calls.empty());
    runPosted();
    ASSERT_EQ(results.size(), 5u);
    for (const Result& result : results)
        EXPECT_EQ(result.errorName, Operations::kSystemError);
    EXPECT_EQ(results[0].errorMessage, "The device is no longer available.");
    EXPECT_EQ(results[2].errorMessage, "The volume holds the running system.");
    EXPECT_EQ(results[3].errorMessage, "The disks of the running system could not be found.");
}

TEST_F(OperationsTest, AVolumeOnTheSystemDisk)
{
    const std::string windows = addDataPartitionToSystemDisk(model);
    model.changeProperties(windows, interface::kFilesystem,
                           {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>{encodeBytes("/media/u/W")})}});
    std::vector<Result> refused;
    const auto collect = [&](const Result& r) { refused.push_back(r); };
    // The partition's own volume goes out.
    operations.unmount(windows);
    backend.replyAll();
    runPosted();
    operations.setLabel(windows, "Win");
    backend.replyAll();
    runPosted();
    operations.repair(windows);
    backend.replyAll();
    runPosted();
    operations.format(windows, "ntfs", {});
    // In place: the running disk's table keeps the partition's type.
    EXPECT_FALSE(backend.calls.back().arguments.at(1).value<VariantMap>().at("update-partition-type").value<bool>());
    backend.replyAll();
    runPosted();
    EXPECT_EQ(backend.calls.size(), 4u);
    // Elsewhere the type follows the filesystem.
    operations.format(sample::kData, "ext4", {});
    EXPECT_TRUE(backend.calls.back().arguments.at(1).value<VariantMap>().at("update-partition-type").value<bool>());
    backend.replyAll();
    runPosted();
    // The table, the whole disk and the drive do not.
    operations.deletePartition(windows, collect);
    operations.resizePartition(windows, 1, collect);
    operations.resizeFilesystem(windows, 1, collect);
    operations.setPartitionType(windows, "0x83", collect);
    operations.openForRestore(windows, collect);
    operations.createPartition(sample::kNvme, 0, 1, {}, {}, collect);
    operations.format(sample::kNvme, "gpt", {}, collect);
    EXPECT_EQ(backend.calls.size(), 5u);
    runPosted();
    ASSERT_EQ(refused.size(), 7u);
    for (const Result& result : refused) {
        EXPECT_EQ(result.errorName, Operations::kSystemError);
        EXPECT_EQ(result.errorMessage, "The disk holds the running system.");
    }
}

TEST_F(OperationsTest, PromptsHaveTimeToBeAnswered)
{
    operations.mount(sample::kData);
    operations.unlock(sample::kLocked, "x");
    operations.loopSetup("/dev/null", true);
    ASSERT_EQ(backend.calls.size(), 3u);
    for (const Call& call : backend.calls)
        EXPECT_GE(call.timeout, std::chrono::hours(1)) << call.method;
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, Format)
{
    FormatOptions formatting;
    formatting.label = "Backup";
    formatting.passphrase = "secret";
    formatting.encryptionType = "luks2";
    operations.format(sample::kData, "exfat", formatting);
    ASSERT_EQ(backend.calls.size(), 1u);
    const Call& call = backend.calls.front();
    EXPECT_EQ(call.interface, interface::kBlock);
    EXPECT_EQ(call.method, "Format");
    EXPECT_EQ(call.arguments[0].value<std::string>(), "exfat");
    const VariantMap map = options(call, 1);
    EXPECT_EQ(map.at("label").value<std::string>(), "Backup");
    EXPECT_EQ(map.at("encrypt.passphrase").value<std::string>(), "secret");
    EXPECT_EQ(map.at("encrypt.type").value<std::string>(), "luks2");
    EXPECT_FALSE(map.at("tear-down").value<bool>());
    EXPECT_TRUE(map.at("update-partition-type").value<bool>());
    EXPECT_TRUE(map.at("take-ownership").value<bool>());
    EXPECT_FALSE(map.contains("erase"));
    EXPECT_GE(call.timeout, std::chrono::hours(1));
    backend.replyAll();
    runPosted();

    operations.format(sample::kCard, "vfat", {.label = "CARD", .erase = "zero", .takeOwnership = false});
    const VariantMap card = options(backend.calls.back(), 1);
    EXPECT_EQ(card.at("erase").value<std::string>(), "zero");
    EXPECT_FALSE(card.at("take-ownership").value<bool>());
    EXPECT_FALSE(card.contains("encrypt.passphrase"));
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, TearsDownOnlyWhatHasSystemEntries)
{
    using Entries = std::vector<std::tuple<std::string, VariantMap>>;
    const auto tearsDown = [&](const std::string& block) {
        operations.format(block, "ext4", {});
        const bool tearDown = options(backend.calls.back(), 1).at("tear-down").value<bool>();
        backend.replyAll();
        runPosted();
        return tearDown;
    };
    EXPECT_FALSE(tearsDown(sample::kUsb));

    // An entry on the cleartext block of an unlocked partition counts for
    // the partition and its disk.
    model.changeProperties(sample::kCleartext, interface::kBlock,
                           {{"Configuration", Variant(Entries{{"fstab", VariantMap{}}})}});
    EXPECT_TRUE(tearsDown(sample::kUnlocked));
    EXPECT_TRUE(tearsDown(sample::kUsb));
    EXPECT_FALSE(tearsDown(sample::kUnformatted));

    // A locked partition keeps its cleartext's entries itself.
    model.changeProperties(sample::kLocked, interface::kEncrypted,
                           {{"ChildConfiguration", Variant(Entries{{"fstab", VariantMap{}}})}});
    EXPECT_TRUE(tearsDown(sample::kLocked));

    model.changeProperties(sample::kCard, interface::kBlock,
                           {{"Configuration", Variant(Entries{{"crypttab", VariantMap{}}})}});
    EXPECT_TRUE(tearsDown(sample::kCard));

    // A new partition has nothing to tear down.
    operations.createPartitionAndFormat(sample::kUsb, 1, 2, "type", "Name", "ext4", {});
    EXPECT_FALSE(options(backend.calls.back(), 6).at("tear-down").value<bool>());
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, Partitions)
{
    std::optional<Result> created;
    operations.createPartition(sample::kSata, 600000000000, 100000000000, "0fc63daf-8483-4772-8e79-3d69d8477de4",
                               "Linux", [&](const Result& r) { created = r; });
    ASSERT_EQ(backend.calls.size(), 1u);
    const Call& call = backend.calls.front();
    EXPECT_EQ(call.interface, interface::kPartitionTable);
    EXPECT_EQ(call.method, "CreatePartition");
    EXPECT_EQ(call.arguments[0].value<uint64_t>(), 600000000000u);
    EXPECT_EQ(call.arguments[1].value<uint64_t>(), 100000000000u);
    EXPECT_EQ(call.arguments[3].value<std::string>(), "Linux");
    backend.reply(0, value(Variant(ObjectPath{TreeBuilder::blockPath("sda2")})));
    runPosted();
    ASSERT_TRUE(created);
    EXPECT_EQ(created->objectPath(), TreeBuilder::blockPath("sda2"));

    operations.createPartitionAndFormat(sample::kUsb, 1, 2, "type", "Name", "ext4", {.label = "New"});
    EXPECT_EQ(backend.calls.back().arguments.size(), 7u);
    EXPECT_EQ(backend.calls.back().arguments[5].value<std::string>(), "ext4");
    EXPECT_EQ(options(backend.calls.back(), 6).at("label").value<std::string>(), "New");
    backend.replyAll();
    runPosted();

    operations.deletePartition(sample::kUnformatted);
    EXPECT_EQ(backend.calls.back().interface, interface::kPartition);
    EXPECT_EQ(backend.calls.back().method, "Delete");
    EXPECT_TRUE(options(backend.calls.back(), 0).at("tear-down").value<bool>());
    backend.replyAll();
    runPosted();

    operations.resizePartition(sample::kMbrLinux, 50000000000);
    EXPECT_EQ(backend.calls.back().method, "Resize");
    EXPECT_EQ(backend.calls.back().arguments[0].value<uint64_t>(), 50000000000u);
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, CheckAndRepair)
{
    std::optional<Result> checked;
    operations.check(sample::kData, [&](const Result& r) { checked = r; });
    EXPECT_EQ(backend.calls.back().method, "Check");
    backend.reply(0, value(Variant(true)));
    runPosted();
    ASSERT_TRUE(checked);
    EXPECT_TRUE(checked->boolean());

    operations.repair(sample::kData);
    EXPECT_EQ(backend.calls.back().method, "Repair");
    backend.reply(0, error("org.freedesktop.UDisks2.Error.Failed", "fsck failed"));
    runPosted();
}

TEST_F(OperationsTest, EjectAnEncryptedDisk)
{
    std::optional<Result> result;
    operations.ejectDisk(sample::kUsb, [&](const Result& r) { result = r; });
    // The cleartext block is unmounted, its encrypted block locked, then the
    // drive powered off, one after the other.
    ASSERT_EQ(backend.calls.size(), 1u);
    EXPECT_EQ(backend.calls[0].path, sample::kCleartext);
    EXPECT_EQ(backend.calls[0].method, "Unmount");
    EXPECT_TRUE(operations.isBusy(sample::kUsb));
    backend.reply(0);
    runPosted();
    ASSERT_EQ(backend.calls.size(), 2u);
    EXPECT_EQ(backend.calls[1].path, sample::kUnlocked);
    EXPECT_EQ(backend.calls[1].method, "Lock");
    backend.reply(0);
    runPosted();
    ASSERT_EQ(backend.calls.size(), 3u);
    EXPECT_EQ(backend.calls[2].path, sample::kUsbDrive);
    EXPECT_EQ(backend.calls[2].interface, interface::kDrive);
    EXPECT_EQ(backend.calls[2].method, "PowerOff");
    EXPECT_FALSE(result);
    backend.reply(0);
    runPosted();
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->ok());
    EXPECT_FALSE(operations.isBusy(sample::kUsb));
}

TEST_F(OperationsTest, EjectStopsAtAFailure)
{
    std::optional<Result> result;
    operations.ejectDisk(sample::kStick, [&](const Result& r) { result = r; });
    ASSERT_EQ(backend.calls.size(), 1u);
    EXPECT_EQ(backend.calls[0].method, "Unmount");
    backend.reply(0, error("org.freedesktop.UDisks2.Error.DeviceBusy", "target is busy"));
    runPosted();
    EXPECT_EQ(backend.calls.size(), 1u);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->errorName, "org.freedesktop.UDisks2.Error.DeviceBusy");
    EXPECT_EQ(result->errorMessage, "target is busy");

    // Then all the way to Eject.
    backend.calls.clear();
    operations.ejectDisk(sample::kStick);
    backend.reply(0);
    runPosted();
    ASSERT_EQ(backend.calls.size(), 2u);
    EXPECT_EQ(backend.calls[1].method, "Eject");
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, DetachADiskImage)
{
    operations.ejectDisk(sample::kImage);
    ASSERT_EQ(backend.calls.size(), 1u);
    EXPECT_EQ(backend.calls[0].method, "Unmount");
    backend.reply(0);
    runPosted();
    ASSERT_EQ(backend.calls.size(), 2u);
    EXPECT_EQ(backend.calls[1].interface, interface::kLoop);
    EXPECT_EQ(backend.calls[1].method, "Delete");
    EXPECT_EQ(backend.calls[1].path, sample::kImage);
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, Unlock)
{
    std::optional<Result> result;
    operations.unlock(sample::kLocked, "passphrase", [&](const Result& r) { result = r; });
    EXPECT_EQ(backend.calls.back().interface, interface::kEncrypted);
    EXPECT_EQ(backend.calls.back().arguments[0].value<std::string>(), "passphrase");
    backend.reply(0, value(Variant(ObjectPath{TreeBuilder::blockPath("dm_2d1")})));
    runPosted();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->objectPath(), TreeBuilder::blockPath("dm_2d1"));
}

TEST_F(OperationsTest, LoopSetup)
{
    const std::filesystem::path file =
            std::filesystem::temp_directory_path() / ("disk-utility-image-" + std::to_string(::getpid()) + ".img");
    std::ofstream(file) << std::string(4096, '\0');
    operations.loopSetup(file.string(), true);
    ASSERT_EQ(backend.calls.size(), 1u);
    const Call& call = backend.calls.front();
    EXPECT_EQ(call.path, kManagerPath);
    EXPECT_EQ(call.method, "LoopSetup");
    EXPECT_EQ(call.arguments[0].signature(), "h");
    EXPECT_TRUE(call.arguments[0].value<UnixFd>().isValid());
    EXPECT_TRUE(options(call, 1).at("read-only").value<bool>());
    backend.replyAll();
    runPosted();
    std::filesystem::remove(file);

    std::optional<Result> missing;
    operations.loopSetup("/nonexistent/image.iso", true, [&](const Result& r) { missing = r; });
    EXPECT_EQ(backend.calls.size(), 1u);
    runPosted();
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing->ok());
}

TEST_F(OperationsTest, Smart)
{
    operations.smartUpdate(sample::kNvmeDrive);
    EXPECT_EQ(backend.calls.back().interface, interface::kNvmeController);
    operations.smartGetAttributes(sample::kSataDrive);
    EXPECT_EQ(backend.calls.back().interface, interface::kAta);
    EXPECT_EQ(backend.calls.back().method, "SmartGetAttributes");
    backend.replyAll();
    runPosted();
}

TEST_F(OperationsTest, Availability)
{
    backend.responder = [](const Call& call) -> std::optional<Result> {
        if (call.method == "CanFormat")
            return value(Variant(std::tuple<bool, std::string>(false, "mkfs.exfat")));
        if (call.method == "CanResize")
            return value(Variant(std::tuple<bool, uint64_t, std::string>(true, 22, "")));
        return std::nullopt;
    };
    std::optional<std::pair<bool, std::string>> format;
    operations.canFormat("exfat", [&](bool available, const std::string& missing) { format = {available, missing}; });
    std::optional<uint64_t> modes;
    operations.canResize("ext4", [&](bool available, uint64_t flags, const std::string&) {
        if (available)
            modes = flags;
    });
    runPosted();
    ASSERT_TRUE(format);
    EXPECT_FALSE(format->first);
    EXPECT_EQ(format->second, "mkfs.exfat");
    ASSERT_TRUE(modes);
    EXPECT_EQ(*modes, 22u);
    EXPECT_EQ(backend.calls.front().arguments[0].value<std::string>(), "exfat");
}

TEST_F(OperationsTest, NothingAfterTheOperationsGo)
{
    bool called = false;
    auto own = std::make_unique<Operations>(model, backend);
    own->mount(sample::kData, [&](const Result&) { called = true; });
    own.reset();
    backend.replyAll();
    runPosted();
    EXPECT_FALSE(called);
}

TEST_F(OperationsTest, CancelAJob)
{
    operations.cancelJob("/org/freedesktop/UDisks2/jobs/3");
    EXPECT_EQ(backend.calls.back().interface, interface::kJob);
    EXPECT_EQ(backend.calls.back().method, "Cancel");
    backend.replyAll();
    runPosted();
    EXPECT_TRUE(error("org.freedesktop.UDisks2.Error.Cancelled").cancelled());
}
