// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "actions.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

using namespace udisks;
using namespace udisks::fake;

static constexpr uint64_t kMiB = 1024 * 1024;
static constexpr uint64_t kGB = 1000 * 1000 * 1000;

class ActionsTest : public testing::Test
{
protected:
    void SetUp() override
    {
        loadSample(model);
        tree = buildTree(model, ViewMode::AllDevices);
    }

    Selection select(const std::string& id)
    {
        const Node* found = findNode(tree, id);
        EXPECT_NE(found, nullptr) << id;
        return found ? Selection(*found) : Selection();
    }

    bool enabled(Action action, const std::string& id)
    {
        return actionState(action, model, &operations, select(id)).enabled;
    }

    properdesktop::base::EventLoop loop;
    Model model;
    FakeBackend backend;
    Operations operations{model, backend};
    std::vector<SectionNodes> tree;
};

TEST_F(ActionsTest, WithoutASelection)
{
    for (Action action : {Action::View, Action::AttachImage, Action::NewBlankImage})
        EXPECT_TRUE(actionState(action, model, &operations, std::nullopt).enabled) << actionName(action);
    for (Action action : {Action::Info, Action::Erase, Action::Mount, Action::Partition, Action::Eject})
        EXPECT_FALSE(actionState(action, model, &operations, std::nullopt).enabled) << actionName(action);
}

TEST_F(ActionsTest, TheSystemVolumeAndDisk)
{
    const std::string root = "v:" + sample::kRoot;
    for (Action action : {Action::Erase, Action::Restore, Action::Unmount, Action::FirstAid, Action::RemoveVolume})
        EXPECT_FALSE(enabled(action, root)) << actionName(action);
    EXPECT_EQ(actionState(Action::Erase, model, &operations, select(root)).reason,
              "The volume holds the running system.");
    EXPECT_TRUE(enabled(Action::Info, root));
    EXPECT_FALSE(enabled(Action::Rename, root));
    EXPECT_TRUE(enabled(Action::NewImageFromSelection, root));
    EXPECT_FALSE(enabled(Action::Mount, root));

    const std::string efi = "v:" + sample::kEfi;
    EXPECT_FALSE(enabled(Action::Erase, efi));
    EXPECT_FALSE(enabled(Action::Unmount, efi));

    const std::string nvme = "d:" + sample::kNvme;
    for (Action action : {Action::Erase, Action::Restore, Action::Partition, Action::AddVolume, Action::Eject,
                          Action::Unmount, Action::FirstAid})
        EXPECT_FALSE(enabled(action, nvme)) << actionName(action);
    EXPECT_EQ(actionState(Action::Partition, model, &operations, select(nvme)).reason,
              "The disk holds the running system.");
}

TEST_F(ActionsTest, AnUnmountedVolume)
{
    const std::string data = "v:" + sample::kData;
    for (Action action : {Action::Mount, Action::Erase, Action::Restore, Action::FirstAid, Action::RemoveVolume,
                          Action::Rename, Action::Info})
        EXPECT_TRUE(enabled(action, data)) << actionName(action);
    EXPECT_FALSE(enabled(Action::Unmount, data));
    // Partition and Add Volume work on its disk, which has room.
    EXPECT_TRUE(enabled(Action::Partition, data));
    EXPECT_TRUE(enabled(Action::AddVolume, data));
    EXPECT_FALSE(enabled(Action::Eject, data));
}

TEST_F(ActionsTest, RemovableMedia)
{
    const std::string stick = "v:" + sample::kStickVolume;
    EXPECT_TRUE(enabled(Action::Unmount, stick));
    EXPECT_FALSE(enabled(Action::Mount, stick));
    EXPECT_TRUE(enabled(Action::Eject, stick));
    EXPECT_TRUE(enabled(Action::Eject, "d:" + sample::kStick));
    EXPECT_TRUE(enabled(Action::Unmount, "d:" + sample::kStick));
    EXPECT_FALSE(enabled(Action::Mount, "d:" + sample::kStick));
    // Filled to the end.
    EXPECT_FALSE(enabled(Action::AddVolume, "d:" + sample::kStick));

    // Powered off rather than ejected.
    EXPECT_TRUE(enabled(Action::Eject, "d:" + sample::kUsb));
    // Neither.
    EXPECT_FALSE(enabled(Action::Eject, "d:" + sample::kCard));
}

TEST_F(ActionsTest, EncryptedVolumes)
{
    const std::string locked = "v:" + sample::kLocked;
    EXPECT_TRUE(enabled(Action::Mount, locked));
    EXPECT_FALSE(enabled(Action::Unmount, locked));
    EXPECT_FALSE(enabled(Action::FirstAid, locked));
    EXPECT_FALSE(enabled(Action::Rename, locked));
    EXPECT_TRUE(enabled(Action::Erase, locked));

    const std::string unlocked = "v:" + sample::kUnlocked;
    EXPECT_TRUE(enabled(Action::Unmount, unlocked));
    EXPECT_FALSE(enabled(Action::Mount, unlocked));
    EXPECT_TRUE(enabled(Action::Rename, unlocked));

    // Unlocked and unmounted: Unmount locks it.
    model.changeProperties(sample::kCleartext, interface::kFilesystem,
                           {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>())}});
    EXPECT_TRUE(enabled(Action::Unmount, unlocked));
    EXPECT_TRUE(enabled(Action::Mount, unlocked));
}

TEST_F(ActionsTest, PartitionsAndDisksWithoutVolumes)
{
    const std::string unformatted = "p:" + sample::kUnformatted;
    EXPECT_TRUE(enabled(Action::Erase, unformatted));
    EXPECT_TRUE(enabled(Action::RemoveVolume, unformatted));
    EXPECT_FALSE(enabled(Action::Mount, unformatted));
    EXPECT_FALSE(enabled(Action::FirstAid, unformatted));
    EXPECT_FALSE(enabled(Action::Rename, unformatted));

    const std::string card = "d:" + sample::kCard;
    EXPECT_TRUE(enabled(Action::Erase, card));
    EXPECT_TRUE(enabled(Action::Partition, card));
    // No partition table to add to.
    EXPECT_FALSE(enabled(Action::AddVolume, card));
    EXPECT_FALSE(enabled(Action::Mount, card));
    EXPECT_FALSE(enabled(Action::RemoveVolume, card));

    const std::string extended = "c:" + sample::kExtended;
    EXPECT_FALSE(enabled(Action::Erase, extended));
    EXPECT_FALSE(enabled(Action::NewImageFromSelection, extended));
    EXPECT_TRUE(enabled(Action::RemoveVolume, extended));
}

TEST_F(ActionsTest, ADiskImage)
{
    const std::string image = "v:" + sample::kImage;
    EXPECT_TRUE(enabled(Action::Eject, image));
    EXPECT_TRUE(enabled(Action::Unmount, image));
    // Attached read-only.
    EXPECT_FALSE(enabled(Action::Erase, image));
    EXPECT_FALSE(enabled(Action::Rename, image));
    EXPECT_FALSE(enabled(Action::Partition, "d:" + sample::kImage));
}

TEST_F(ActionsTest, SwapInUse)
{
    model.changeProperties(sample::kLogicalSwap, interface::kSwapspace, {{"Active", Variant(true)}});
    EXPECT_FALSE(enabled(Action::Erase, "d:" + sample::kMbr));
    EXPECT_FALSE(enabled(Action::Partition, "d:" + sample::kMbr));
    EXPECT_FALSE(enabled(Action::RemoveVolume, "c:" + sample::kExtended));
    EXPECT_FALSE(enabled(Action::Eject, "d:" + sample::kMbr));
    // Its neighbour can still go.
    EXPECT_TRUE(enabled(Action::Erase, "v:" + sample::kMbrLinux));
}

TEST_F(ActionsTest, BusyWhileAJobRuns)
{
    TreeBuilder jobs;
    const std::string job = jobs.addJob("7", "filesystem-check", {sample::kData});
    model.addInterfaces(job, jobs.objects().at(job));
    const ActionState state = actionState(Action::Erase, model, &operations, select("v:" + sample::kData));
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(state.reason, "An operation on this disk is in progress.");
    EXPECT_FALSE(enabled(Action::Erase, "d:" + sample::kSata));
    EXPECT_TRUE(enabled(Action::Info, "v:" + sample::kData));
    model.removeInterfaces(job, {interface::kJob});
    EXPECT_TRUE(enabled(Action::Erase, "v:" + sample::kData));
}

TEST_F(ActionsTest, BusyWhileAnOperationRuns)
{
    operations.mount(sample::kData);
    EXPECT_TRUE(operations.isBusy(sample::kData));
    EXPECT_FALSE(enabled(Action::Mount, "v:" + sample::kData));
    EXPECT_FALSE(enabled(Action::Partition, "d:" + sample::kSata));
    EXPECT_TRUE(enabled(Action::Unmount, "v:" + sample::kStickVolume));
    backend.replyAll();
    runPosted();
    EXPECT_FALSE(operations.isBusy(sample::kData));
    EXPECT_TRUE(enabled(Action::Mount, "v:" + sample::kData));
}

TEST_F(ActionsTest, FreeSpace)
{
    // sda1 takes the first 500 GB after the first MiB.
    const uint64_t sataEnd = (1000204886016 - kMiB) / kMiB * kMiB;
    const uint64_t sataStart = (kMiB + 500 * kGB + kMiB - 1) / kMiB * kMiB;
    EXPECT_EQ(largestFreeSpace(model, sample::kSata), sataEnd - sataStart);
    EXPECT_EQ(largestFreeSpace(model, sample::kStick), 0u);
    EXPECT_EQ(largestFreeSpace(model, sample::kNvme), 0u);
    EXPECT_EQ(largestFreeSpace(model, sample::kCard), 0u);
    EXPECT_GT(largestFreeSpace(model, sample::kUsb), 100 * kGB);
}

TEST_F(ActionsTest, VolumesOnADisk)
{
    std::vector<std::string> paths;
    for (const Object* volume : volumesOnDisk(model, sample::kUsb))
        paths.push_back(volume->path);
    EXPECT_EQ(paths, (std::vector<std::string>{sample::kCleartext, sample::kLocked}));
    EXPECT_EQ(volumesOnDisk(model, sample::kImage).size(), 1u);
    EXPECT_TRUE(volumesOnDisk(model, sample::kCard).empty());
}

TEST_F(ActionsTest, Dispatcher)
{
    ActionDispatcher dispatcher(model, operations);
    int changes = 0;
    auto subscription = dispatcher.statesChanged.connect([&] { ++changes; });

    dispatcher.setSelection(select("v:" + sample::kData));
    EXPECT_EQ(changes, 1);
    // Nothing handles Erase yet.
    EXPECT_FALSE(dispatcher.isEnabled(Action::Erase));
    EXPECT_FALSE(dispatcher.trigger(Action::Erase));

    Selection erased;
    dispatcher.setHandler(Action::Erase, [&](const Selection& selection) { erased = selection; });
    EXPECT_EQ(changes, 2);
    EXPECT_TRUE(dispatcher.isEnabled(Action::Erase));
    EXPECT_TRUE(dispatcher.trigger(Action::Erase));
    ASSERT_TRUE(erased);
    EXPECT_EQ(erased->block, sample::kData);

    // The same selection again changes nothing.
    dispatcher.setSelection(select("v:" + sample::kData));
    EXPECT_EQ(changes, 2);

    // The disks change.
    model.changeProperties(sample::kData, interface::kBlock, {{"IdLabel", Variant(std::string("New"))}});
    EXPECT_EQ(changes, 3);

    // A disabled action does not run.
    erased.reset();
    dispatcher.setSelection(select("v:" + sample::kRoot));
    EXPECT_FALSE(dispatcher.trigger(Action::Erase));
    EXPECT_FALSE(erased);

    // A handler may replace itself while it runs.
    int views = 0;
    dispatcher.setHandler(Action::View, [&](const Selection&) {
        ++views;
        dispatcher.setHandler(Action::View, {});
    });
    EXPECT_TRUE(dispatcher.trigger(Action::View));
    EXPECT_EQ(views, 1);
    EXPECT_FALSE(dispatcher.hasHandler(Action::View));

    // Busy, through the operations.
    dispatcher.setHandler(Action::Mount, [](const Selection&) {});
    dispatcher.setSelection(select("v:" + sample::kData));
    EXPECT_TRUE(dispatcher.isEnabled(Action::Mount));
    const int before = changes;
    operations.mount(sample::kData);
    EXPECT_GT(changes, before);
    EXPECT_FALSE(dispatcher.isEnabled(Action::Mount));
    backend.replyAll();
    runPosted();
}

TEST_F(ActionsTest, AnExtendedPartitionWithAVolumeInUse)
{
    const std::string extended = "c:" + sample::kExtended;
    EXPECT_TRUE(enabled(Action::RemoveVolume, extended));
    model.changeProperties(sample::kLogicalFat, interface::kFilesystem,
                           {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>{encodeBytes("/media/u/S")})}});
    const ActionState state = actionState(Action::RemoveVolume, model, &operations, select(extended));
    EXPECT_FALSE(state.enabled);
    EXPECT_EQ(state.reason, "A partition in it is mounted or unlocked.");
    std::vector<std::string> logical;
    for (const Object* partition : model.logicalPartitionsOf(sample::kExtended))
        logical.push_back(partition->path);
    EXPECT_EQ(logical, (std::vector<std::string>{sample::kLogicalFat, sample::kLogicalSwap}));
}

TEST_F(ActionsTest, UnknownSystemDisablesEveryChange)
{
    model.setSystemDevices({});
    for (Action action : {Action::Erase, Action::Partition, Action::Unmount, Action::RemoveVolume, Action::Rename,
                          Action::Eject, Action::FirstAid, Action::Restore}) {
        const ActionState state = actionState(action, model, &operations, select("v:" + sample::kStickVolume));
        EXPECT_FALSE(state.enabled) << actionName(action);
        EXPECT_EQ(state.reason, "The disks of the running system could not be found.") << actionName(action);
    }
    EXPECT_TRUE(enabled(Action::Info, "v:" + sample::kStickVolume));
}

TEST_F(ActionsTest, AVolumeOnTheSystemDisk)
{
    const std::string windows = addDataPartitionToSystemDisk(model);
    tree = buildTree(model, ViewMode::AllDevices);
    const std::string id = "v:" + windows;
    // Its own volume: mounted, unmounted, renamed, checked, erased in place.
    EXPECT_TRUE(enabled(Action::Mount, id));
    for (Action action : {Action::Rename, Action::FirstAid, Action::Erase, Action::Info})
        EXPECT_TRUE(enabled(action, id)) << actionName(action);
    model.changeProperties(windows, interface::kFilesystem,
                           {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>{encodeBytes("/media/u/W")})}});
    EXPECT_TRUE(enabled(Action::Unmount, id));
    // The disk's table and the drive stay as they are.
    for (Action action : {Action::RemoveVolume, Action::Partition, Action::AddVolume, Action::Eject}) {
        const ActionState state = actionState(action, model, &operations, select(id));
        EXPECT_FALSE(state.enabled) << actionName(action);
        EXPECT_EQ(state.reason, "The disk holds the running system.") << actionName(action);
    }
    // Said plainly: a copy brings its source's filesystem UUID.
    const ActionState restore = actionState(Action::Restore, model, &operations, select(id));
    EXPECT_FALSE(restore.enabled);
    EXPECT_NE(restore.reason.find("could start from the wrong one"), std::string::npos);
    EXPECT_FALSE(enabled(Action::Erase, "d:" + sample::kNvme));
    EXPECT_FALSE(enabled(Action::FirstAid, "d:" + sample::kNvme));
}

TEST_F(ActionsTest, NamesAndTitles)
{
    EXPECT_EQ(actionName(Action::FirstAid), "firstAid");
    EXPECT_EQ(actionTitle(Action::Erase), "Erase…");
    EXPECT_EQ(kToolbarActions.front(), Action::View);
    EXPECT_EQ(kToolbarActions.back(), Action::Info);
}
