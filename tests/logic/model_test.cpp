// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "udisks/model.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace udisks;
using namespace udisks::fake;
using properdesktop::base::dbus::ObjectPath;
using properdesktop::base::dbus::Type;

static Variant mountPoints(const std::vector<std::string>& points)
{
    std::vector<std::vector<uint8_t>> bytes;
    for (const std::string& point : points)
        bytes.push_back(encodeBytes(point));
    return Variant(bytes);
}

TEST(ModelTest, ParsesTheObjects)
{
    Model model;
    EXPECT_FALSE(model.isReady());
    loadSample(model);
    ASSERT_TRUE(model.isReady());

    const Object* root = model.object(sample::kRoot);
    ASSERT_NE(root, nullptr);
    ASSERT_TRUE(root->block && root->partition && root->filesystem);
    EXPECT_EQ(root->block->device, "/dev/nvme0n1p2");
    EXPECT_EQ(root->block->idType, "ext4");
    EXPECT_EQ(root->block->idLabel, "ProperDesktop");
    EXPECT_EQ(root->block->drive, sample::kNvmeDrive);
    EXPECT_TRUE(root->block->cryptoBackingDevice.empty());
    EXPECT_EQ(root->partition->number, 2u);
    EXPECT_EQ(root->partition->table, sample::kNvme);
    EXPECT_EQ(root->filesystem->mountPoints, std::vector<std::string>{"/"});
    EXPECT_FALSE(root->drive);

    const Object* drive = model.object(sample::kNvmeDrive);
    ASSERT_TRUE(drive && drive->drive && drive->nvme);
    EXPECT_EQ(drive->drive->model, "YMTC PC005 512GB");
    EXPECT_EQ(drive->nvme->smartTemperature, 302);
    EXPECT_FALSE(drive->ata);

    const Object* image = model.object(sample::kImage);
    ASSERT_TRUE(image && image->loop);
    EXPECT_EQ(image->loop->backingFile, "/home/u/Downloads/debian.iso");
    EXPECT_TRUE(image->block->readOnly);
    EXPECT_TRUE(image->block->drive.empty());

    // The raw properties stay for the Info window.
    EXPECT_TRUE(root->properties.at(interface::kBlock).contains("IdUUID"));
}

TEST(ModelTest, ReadsValuesStillInTheirVariant)
{
    Model model;
    VariantMap block = {
        {"Size", Type<Variant>::toVariant(Variant(uint64_t(42)))},
        {"IdLabel", Type<Variant>::toVariant(Variant(std::string("Wrapped")))},
        {"Drive", Type<Variant>::toVariant(Variant(ObjectPath{"/"}))},
    };
    model.reset({{"/b", {{interface::kBlock, block}}}});
    const Object* object = model.object("/b");
    ASSERT_TRUE(object && object->block);
    EXPECT_EQ(object->block->size, 42u);
    EXPECT_EQ(object->block->idLabel, "Wrapped");
    EXPECT_TRUE(object->block->drive.empty());
}

TEST(ModelTest, InterfacesAddedAndRemoved)
{
    Model model;
    loadSample(model);
    std::vector<std::string> added;
    std::vector<std::string> removed;
    int changes = 0;
    auto a = model.objectAdded.connect([&](const std::string& path) { added.push_back(path); });
    auto r = model.objectRemoved.connect([&](const std::string& path) { removed.push_back(path); });
    auto c = model.changed.connect([&] { ++changes; });

    TreeBuilder plugged;
    const std::string drive = plugged.addDrive("Kingston", {.model = "DataTraveler", .size = 8000000000,
                                                            .connectionBus = "usb", .removable = true});
    const std::string disk = plugged.addDisk("sdf", drive, {.size = 8000000000});
    model.addInterfaces(drive, plugged.objects().at(drive));
    model.addInterfaces(disk, plugged.objects().at(disk));
    EXPECT_EQ(added, (std::vector<std::string>{drive, disk}));
    EXPECT_EQ(changes, 2);
    ASSERT_NE(model.wholeBlock(drive), nullptr);
    EXPECT_EQ(model.wholeBlock(drive)->path, disk);

    // A filesystem made on it: the object stays, an interface comes.
    model.addInterfaces(disk, {{interface::kFilesystem, {{"MountPoints", mountPoints({})}}}});
    EXPECT_EQ(added.size(), 2u);
    EXPECT_TRUE(model.object(disk)->filesystem);
    model.removeInterfaces(disk, {interface::kFilesystem});
    EXPECT_FALSE(model.object(disk)->filesystem);
    EXPECT_TRUE(removed.empty());

    // Unplugged: the last interface takes the object along.
    model.removeInterfaces(disk, {interface::kBlock});
    model.removeInterfaces(drive, {interface::kDrive});
    EXPECT_EQ(removed, (std::vector<std::string>{disk, drive}));
    EXPECT_EQ(model.object(disk), nullptr);
    EXPECT_EQ(model.object(drive), nullptr);
}

TEST(ModelTest, PropertiesChanged)
{
    Model model;
    loadSample(model);
    std::vector<std::string> changed;
    auto subscription = model.objectChanged.connect([&](const std::string& path) { changed.push_back(path); });

    model.changeProperties(sample::kData, interface::kFilesystem, {{"MountPoints", mountPoints({"/media/u/Data"})}});
    EXPECT_EQ(model.object(sample::kData)->filesystem->mountPoints, std::vector<std::string>{"/media/u/Data"});
    EXPECT_EQ(changed, std::vector<std::string>{sample::kData});

    model.changeProperties(sample::kData, interface::kBlock, {{"IdLabel", Variant(std::string("Archive"))}});
    EXPECT_EQ(model.object(sample::kData)->block->idLabel, "Archive");
    // Other properties of the interface stay.
    EXPECT_EQ(model.object(sample::kData)->block->idType, "ntfs");

    model.changeProperties(sample::kData, interface::kBlock, {}, {"IdLabel"});
    EXPECT_EQ(model.object(sample::kData)->block->idLabel, "");

    // An object UDisks2 never announced is ignored.
    model.changeProperties("/nowhere", interface::kBlock, {{"Size", Variant(uint64_t(1))}});
    EXPECT_EQ(model.object("/nowhere"), nullptr);
}

TEST(ModelTest, ResetReportsTheDifference)
{
    Model model;
    loadSample(model);
    Model::Objects objects = sampleTree();
    objects.erase(sample::kStickVolume);
    objects[sample::kData][interface::kBlock]["IdLabel"] = Variant(std::string("Renamed"));
    std::vector<std::string> removed;
    std::vector<std::string> changed;
    auto r = model.objectRemoved.connect([&](const std::string& path) { removed.push_back(path); });
    auto c = model.objectChanged.connect([&](const std::string& path) { changed.push_back(path); });
    model.reset(objects);
    EXPECT_EQ(removed, std::vector<std::string>{sample::kStickVolume});
    EXPECT_EQ(changed, std::vector<std::string>{sample::kData});

    // UDisks2 went away.
    model.reset({});
    EXPECT_TRUE(model.objects().empty());
    EXPECT_TRUE(model.isReady());
}

TEST(ModelTest, Relations)
{
    Model model;
    loadSample(model);

    std::vector<std::string> partitions;
    for (const Object* partition : model.partitionsOf(sample::kMbr))
        partitions.push_back(partition->path);
    EXPECT_EQ(partitions, (std::vector<std::string>{sample::kMbrLinux, sample::kExtended, sample::kLogicalFat,
                                                    sample::kLogicalSwap}));

    ASSERT_NE(model.cleartextOf(sample::kUnlocked), nullptr);
    EXPECT_EQ(model.cleartextOf(sample::kUnlocked)->path, sample::kCleartext);
    EXPECT_EQ(model.cleartextOf(sample::kLocked), nullptr);
    EXPECT_EQ(model.contentOf(sample::kUnlocked)->path, sample::kCleartext);
    EXPECT_EQ(model.contentOf(sample::kLocked)->path, sample::kLocked);

    std::vector<std::string> blocks;
    for (const Object* block : model.blocksOfDrive(sample::kUsbDrive))
        blocks.push_back(block->path);
    std::sort(blocks.begin(), blocks.end());
    std::vector<std::string> expected{sample::kUsb, sample::kUnlocked, sample::kLocked, sample::kUnformatted,
                                      sample::kCleartext};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(blocks, expected);

    EXPECT_EQ(model.wholeBlock(sample::kUsbDrive)->path, sample::kUsb);
    EXPECT_EQ(model.wholeBlock({}), nullptr);
}

TEST(ModelTest, JobsFollowTheirObjects)
{
    Model model;
    loadSample(model);
    TreeBuilder jobs;
    const std::string job = jobs.addJob("7", "filesystem-check", {sample::kCleartext}, 0.25);
    model.addInterfaces(job, jobs.objects().at(job));

    ASSERT_EQ(model.jobsFor(sample::kCleartext).size(), 1u);
    EXPECT_EQ(model.jobsFor(sample::kCleartext).front()->job->operation, "filesystem-check");
    // Through the encrypted block and the drive.
    EXPECT_EQ(model.jobsFor(sample::kUnlocked).size(), 1u);
    EXPECT_EQ(model.jobsFor(sample::kUsbDrive).size(), 1u);
    EXPECT_TRUE(model.jobsFor(sample::kStickDrive).empty());

    model.changeProperties(job, interface::kJob, {{"Progress", Variant(0.75)}});
    EXPECT_DOUBLE_EQ(model.jobsFor(sample::kCleartext).front()->job->progress, 0.75);
    model.removeInterfaces(job, {interface::kJob});
    EXPECT_TRUE(model.jobsFor(sample::kCleartext).empty());
}

TEST(ModelTest, Manager)
{
    Model model;
    model.setManager({{"Version", Variant(std::string("2.10.1"))},
                      {"SupportedFilesystems", Variant(std::vector<std::string>{"ext4", "vfat"})},
                      {"DefaultEncryptionType", Variant(std::string("luks2"))}});
    EXPECT_EQ(model.manager().version, "2.10.1");
    EXPECT_EQ(model.manager().supportedFilesystems, (std::vector<std::string>{"ext4", "vfat"}));
    EXPECT_EQ(model.manager().defaultEncryptionType, "luks2");
}
