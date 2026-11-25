// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "actions.h"
#include "udisks/describe.h"
#include "udisks/errors.h"
#include "udisks/holders.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "udisks/properties.h"
#include "udisks/smart.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <tuple>

#include <unistd.h>

using namespace udisks;
using namespace udisks::fake;
using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

namespace fs = std::filesystem;

class VolumesTest : public testing::Test
{
protected:
    void SetUp() override
    {
        loadSample(model);
        tree = buildTree(model, ViewMode::AllDevices);
    }

    Node node(const std::string& id)
    {
        const Node* found = findNode(tree, id);
        EXPECT_NE(found, nullptr) << id;
        return found ? *found : Node();
    }

    const PropertyGroup* group(const std::vector<PropertyGroup>& groups, const std::string& title)
    {
        for (const PropertyGroup& group : groups) {
            if (group.title == title)
                return &group;
        }
        ADD_FAILURE() << "no group " << title;
        return nullptr;
    }

    std::string value(const PropertyGroup* group, const std::string& key)
    {
        if (!group)
            return {};
        for (const Property& property : group->properties) {
            if (property.key == key)
                return property.value;
        }
        ADD_FAILURE() << "no property " << key << " in " << group->title;
        return {};
    }

    properdesktop::base::EventLoop loop;
    Model model;
    FakeBackend backend;
    Operations operations{model, backend};
    std::vector<SectionNodes> tree;
};

TEST_F(VolumesTest, SmartStatus)
{
    EXPECT_EQ(smartStatus(model, sample::kNvmeDrive), SmartStatus::Verified);
    EXPECT_EQ(smartStatus(model, sample::kSataDrive), SmartStatus::Verified);
    // A USB stick has neither interface.
    EXPECT_EQ(smartStatus(model, sample::kStickDrive), SmartStatus::NotSupported);
    EXPECT_EQ(smartStatus(model, {}), SmartStatus::NotSupported);

    model.changeProperties(sample::kSataDrive, interface::kAta, {{"SmartNumAttributesFailing", Variant(int32_t(2))}});
    EXPECT_EQ(smartStatus(model, sample::kSataDrive), SmartStatus::Failing);
    model.changeProperties(sample::kSataDrive, interface::kAta,
                           {{"SmartNumAttributesFailing", Variant(int32_t(0))}, {"SmartFailing", Variant(true)}});
    EXPECT_EQ(smartStatus(model, sample::kSataDrive), SmartStatus::Failing);
    model.changeProperties(sample::kSataDrive, interface::kAta, {{"SmartEnabled", Variant(false)}});
    EXPECT_EQ(smartStatus(model, sample::kSataDrive), SmartStatus::NotSupported);

    model.changeProperties(sample::kNvmeDrive, interface::kNvmeController,
                           {{"SmartCriticalWarning", Variant(std::vector<std::string>{"temperature"})}});
    EXPECT_EQ(smartStatus(model, sample::kNvmeDrive), SmartStatus::Failing);
    // Never read.
    model.changeProperties(sample::kNvmeDrive, interface::kNvmeController, {{"SmartUpdated", Variant(uint64_t(0))}});
    EXPECT_EQ(smartStatus(model, sample::kNvmeDrive), SmartStatus::NotSupported);

    EXPECT_EQ(smartStatusName(SmartStatus::Verified), "Verified");
    EXPECT_EQ(smartStatusName(SmartStatus::Failing), "Failing");
    EXPECT_EQ(smartStatusName(SmartStatus::NotSupported), "Not Supported");
}

TEST_F(VolumesTest, SmartFormatting)
{
    EXPECT_EQ(formatTemperature(310.15), "37 °C");
    EXPECT_EQ(formatTemperature(302), "29 °C");
    EXPECT_EQ(formatTemperature(0), "");
    EXPECT_EQ(formatPowerOnHours(1), "1 hour");
    EXPECT_EQ(formatPowerOnHours(47), "47 hours");
    EXPECT_EQ(formatPowerOnHours(1108), "46 days (" + formatCount(1108) + " hours)");
    EXPECT_EQ(selftestStatusText("", -1), "");
    EXPECT_EQ(selftestStatusText("success", 0), "Completed without error");
    EXPECT_EQ(selftestStatusText("inprogress", 40), "In progress, 40% left");
    EXPECT_EQ(selftestStatusText("error_read", 0), "Failed: read error");
    EXPECT_EQ(selftestStatusText("aborted_unknown", 0), "Aborted");
}

TEST_F(VolumesTest, AtaAttributes)
{
    using Row = std::tuple<uint8_t, std::string, uint16_t, int32_t, int32_t, int32_t, int64_t, int32_t, VariantMap>;
    const std::vector<Row> rows = {
        {9, "power-on-hours", 0x32, 95, 95, 0, int64_t(3600000) * 5, 2, {}},
        {5, "reallocated-sector-count", 0x33, 5, 5, 10, 12, 3, {}},
        {194, "temperature-celsius-2", 0x22, 63, 40, 0, 310150, 4, {}},
        {231, "", 0x13, 100, 100, 0, 0, 0, {}},
    };
    const std::vector<AtaAttribute> attributes = parseAtaAttributes({Variant(rows)});
    ASSERT_EQ(attributes.size(), 4u);
    EXPECT_EQ(attributeTitle(attributes[0]), "Power-On Hours");
    EXPECT_EQ(attributeValue(attributes[0]), "5 hours");
    EXPECT_FALSE(attributes[0].failing());
    EXPECT_FALSE(attributes[0].preFail());

    EXPECT_EQ(attributeTitle(attributes[1]), "Reallocated Sector Count");
    EXPECT_EQ(attributeValue(attributes[1]), "12 sectors");
    EXPECT_TRUE(attributes[1].failing());
    EXPECT_TRUE(attributes[1].preFail());

    EXPECT_EQ(attributeValue(attributes[2]), "37 °C");
    EXPECT_EQ(attributeTitle(attributes[3]), "Attribute 231");
    EXPECT_EQ(attributeValue(attributes[3]), "");

    EXPECT_TRUE(parseAtaAttributes({}).empty());
    EXPECT_TRUE(parseAtaAttributes({Variant(std::string("nonsense"))}).empty());
}

TEST_F(VolumesTest, NvmeHealth)
{
    const VariantMap health = {
        {"avail_spare", Variant(uint8_t(100))},
        {"spare_thresh", Variant(uint8_t(10))},
        {"percent_used", Variant(uint8_t(3))},
        {"media_errors", Variant(uint64_t(0))},
        {"unsafe_shutdowns", Variant(uint64_t(7))},
    };
    const auto rows = describeNvmeHealth({Variant(health)});
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"Available Spare", "100% (threshold 10%)"},
        {"Wear", "3%"},
        {"Unsafe Shutdowns", "7"},
        {"Media Errors", "0"},
    };
    EXPECT_EQ(rows, expected);
    EXPECT_TRUE(describeNvmeHealth({}).empty());
}

TEST_F(VolumesTest, PropertyTitlesAndValues)
{
    EXPECT_EQ(propertyTitle("IdLabel"), "Label");
    EXPECT_EQ(propertyTitle("ReadOnly"), "Read Only");
    EXPECT_EQ(propertyTitle("SomethingNewHere"), "Something New Here");
    EXPECT_EQ(propertyTitle("MaxUUIDCount"), "Max UUID Count");
    EXPECT_EQ(interfaceTitle(interface::kFilesystem), "File System");
    EXPECT_EQ(interfaceTitle("org.freedesktop.UDisks2.NVMe.Namespace"), "Namespace");

    EXPECT_EQ(formatBytes(512), "512 bytes");
    EXPECT_EQ(formatBytes(512110190592), formatSize(512110190592) + " (" + formatCount(512110190592) + " bytes)");
    EXPECT_EQ(formatDate(0), "");
    EXPECT_FALSE(formatDate(1795176130).empty());

    EXPECT_EQ(partitionTypeName("gpt", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"), "EFI System");
    EXPECT_EQ(partitionTypeName("dos", "0x83"), "Linux");
    EXPECT_EQ(partitionTypeName("dos", "0xc"), "FAT32 (LBA)");
    EXPECT_EQ(partitionTypeName("gpt", "00000000-0000-0000-0000-000000000001"), "");
    EXPECT_EQ(partitionFlagsText("dos", 0x80), "Bootable");
    EXPECT_EQ(partitionFlagsText("gpt", 0), "None");
    EXPECT_EQ(partitionFlagsText("gpt", (uint64_t(1) << 0) | (uint64_t(1) << 62)), "Required, Hidden");
    EXPECT_EQ(partitionFlagsText("gpt", uint64_t(1) << 40), "0x10000000000");
}

TEST_F(VolumesTest, VolumeProperties)
{
    const std::vector<PropertyGroup> groups = describeProperties(model, node("v:" + sample::kData));
    ASSERT_EQ(groups.size(), 3u);
    EXPECT_EQ(groups[0].title, "Block Device");
    EXPECT_EQ(groups[1].title, "Partition");
    EXPECT_EQ(groups[2].title, "File System");
    const PropertyGroup* block = group(groups, "Block Device");
    EXPECT_EQ(value(block, "Device"), "/dev/sda1");
    EXPECT_EQ(value(block, "IdLabel"), "Data");
    EXPECT_EQ(value(block, "IdType"), "NTFS");
    EXPECT_EQ(value(block, "ReadOnly"), "No");
    EXPECT_EQ(value(block, "Drive"), "Seagate ST1000LM035");
    EXPECT_EQ(value(block, "Size"), formatBytes(500 * 1000 * 1000 * uint64_t(1000)));
    const PropertyGroup* partition = group(groups, "Partition");
    EXPECT_EQ(value(partition, "Type"), "Microsoft Basic Data (ebd0a0a2-b9e5-4433-87c0-68b6b72699c7)");
    EXPECT_EQ(value(partition, "Flags"), "None");
    EXPECT_EQ(value(partition, "Table"), "/dev/sda");
    EXPECT_EQ(value(group(groups, "File System"), "MountPoints"), "—");
    // By title.
    for (const PropertyGroup& each : groups) {
        for (size_t i = 1; i < each.properties.size(); ++i)
            EXPECT_LE(each.properties[i - 1].name, each.properties[i].name);
    }
}

TEST_F(VolumesTest, DiskAndUnlockedVolumeProperties)
{
    const std::vector<PropertyGroup> disk = describeProperties(model, node("d:" + sample::kSata));
    const PropertyGroup* drive = group(disk, "Drive");
    EXPECT_EQ(value(drive, "RotationRate"), formatCount(5400) + " RPM");
    EXPECT_EQ(value(drive, "Vendor"), "Seagate");
    EXPECT_EQ(value(group(disk, "S.M.A.R.T."), "SmartTemperature"), "37 °C");
    EXPECT_EQ(value(group(disk, "Partition Map"), "Type"), "GUID Partition Map");
    EXPECT_NE(value(group(disk, "Partition Map"), "Partitions").find("/dev/sda1"), std::string::npos);

    const std::vector<PropertyGroup> nvme = describeProperties(model, node("d:" + sample::kNvme));
    EXPECT_EQ(value(group(nvme, "NVMe Controller"), "SmartTemperature"), "29 °C");
    EXPECT_EQ(value(group(nvme, "Drive"), "RotationRate"), "Solid State");

    const std::vector<PropertyGroup> unlocked = describeProperties(model, node("v:" + sample::kUnlocked));
    EXPECT_EQ(value(group(unlocked, "Encryption"), "CleartextDevice"), "/dev/dm_2d0");
    const PropertyGroup* clear = group(unlocked, "Unlocked Block Device");
    EXPECT_EQ(value(clear, "IdLabel"), "Secret");
    EXPECT_EQ(unlocked.back().title, "File System");
    EXPECT_EQ(value(&unlocked.back(), "MountPoints"), "/media/u/Secret");
}

TEST_F(VolumesTest, Errors)
{
    EXPECT_TRUE(isBusyError("org.freedesktop.UDisks2.Error.DeviceBusy", ""));
    EXPECT_TRUE(isBusyError("org.freedesktop.UDisks2.Error.Failed", "Error unmounting /dev/sdb1: target is busy"));
    EXPECT_FALSE(isBusyError("org.freedesktop.UDisks2.Error.Failed", "No such device"));
    EXPECT_TRUE(isWrongPassphrase("org.freedesktop.UDisks2.Error.Failed",
                                  "Error unlocking /dev/sdc2: Failed to activate device: Operation not permitted"));
    EXPECT_TRUE(isWrongPassphrase("org.freedesktop.UDisks2.Error.Failed", "Incorrect passphrase."));
    EXPECT_FALSE(isWrongPassphrase("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed", "Incorrect passphrase."));
    EXPECT_FALSE(isWrongPassphrase("org.freedesktop.UDisks2.Error.Failed", "Device busy"));
}

TEST_F(VolumesTest, ProcessesUsing)
{
    const fs::path root = fs::temp_directory_path() / ("disk-utility-proc-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const auto process = [&](const std::string& pid, const std::string& name) {
        fs::create_directories(root / pid / "fd");
        std::ofstream(root / pid / "comm") << name << "\n";
        return root / pid;
    };
    fs::create_symlink("/media/u/STICK/docs", process("12", "bash") / "cwd");
    fs::create_symlink("/media/u/STICK/a.txt", process("13", "vim") / "fd" / "3");
    fs::create_symlink("/home/u", process("14", "other") / "cwd");
    std::ofstream(process("15", "player") / "maps")
            << "7f00-7f10 r-xp 00000000 08:11 1234                       /media/u/STICK/lib.so\n";
    fs::create_symlink("/media/u/STICKY", process("16", "nope") / "cwd");
    fs::create_symlink("/media/u/STICK", process("17", "bash") / "cwd");
    fs::create_directories(root / "self");

    EXPECT_EQ(processesUsing({"/media/u/STICK"}, root.string()),
              (std::vector<std::string>{"bash", "player", "vim"}));
    EXPECT_TRUE(processesUsing({"/media/u/Other"}, root.string()).empty());
    // The root is everyone's.
    EXPECT_TRUE(processesUsing({"/"}, root.string()).empty());
    fs::remove_all(root);
}

TEST_F(VolumesTest, LockNeedsAnUnlockedVolume)
{
    const auto state = [&](const std::string& id) {
        return actionState(Action::Lock, model, &operations, Selection(node(id)));
    };
    EXPECT_TRUE(state("v:" + sample::kUnlocked).enabled);
    EXPECT_FALSE(state("v:" + sample::kLocked).enabled);
    EXPECT_FALSE(state("v:" + sample::kStickVolume).enabled);
    EXPECT_FALSE(state("d:" + sample::kUsb).enabled);
    EXPECT_EQ(actionTitle(Action::Lock), "Lock");
    EXPECT_EQ(actionName(Action::Lock), "lock");
}

TEST_F(VolumesTest, RefinementsNarrowTheState)
{
    ActionDispatcher actions(model, operations);
    actions.setHandler(Action::FirstAid, [](const Selection&) {});
    actions.setSelection(node("v:" + sample::kData));
    EXPECT_TRUE(actions.isEnabled(Action::FirstAid));

    int asked = 0;
    actions.setRefinement(Action::FirstAid, [&](const Selection&) {
        ++asked;
        return ActionState{false, "ntfsfix is missing"};
    });
    EXPECT_EQ(actions.state(Action::FirstAid), (ActionState{false, "ntfsfix is missing"}));
    EXPECT_FALSE(actions.trigger(Action::FirstAid));
    // Not asked while the selection rules the action out.
    asked = 0;
    actions.setSelection(node("v:" + sample::kRoot));
    EXPECT_FALSE(actions.isEnabled(Action::FirstAid));
    EXPECT_EQ(asked, 0);

    actions.setRefinement(Action::FirstAid, {});
    actions.setSelection(node("v:" + sample::kData));
    EXPECT_TRUE(actions.isEnabled(Action::FirstAid));
}

TEST_F(VolumesTest, SelftestOnlyWhereTheDriveReportsOne)
{
    Object nvme;
    nvme.nvme = NvmeController{};
    EXPECT_FALSE(canSelftest(nvme));
    nvme.nvme->smartSelftestStatus = "success";
    EXPECT_TRUE(canSelftest(nvme));

    Object ata;
    ata.ata = Ata{.smartSupported = true, .smartEnabled = true, .smartUpdated = 1795176130};
    EXPECT_FALSE(canSelftest(ata));
    ata.ata->smartSelftestStatus = "success";
    EXPECT_TRUE(canSelftest(ata));
    ata.ata->smartEnabled = false;
    EXPECT_FALSE(canSelftest(ata));
    EXPECT_FALSE(canSelftest(Object{}));
}
