// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "dialogs/volume_format.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

#include <tuple>

using namespace dialogs;
using namespace udisks;
using namespace udisks::fake;

TEST(VolumeFormatTest, ListsTheErasableFilesystems)
{
    EXPECT_EQ(erasableFilesystems(), (std::vector<std::string>{"ext4", "exfat", "vfat", "ntfs", "btrfs", "xfs"}));
    EXPECT_EQ(formatName("vfat"), "FAT32 (MS-DOS)");
    EXPECT_EQ(formatName("exfat"), "exFAT");
    EXPECT_EQ(formatName("btrfs"), "Btrfs");
}

TEST(VolumeFormatTest, LabelLengths)
{
    // At the limit, and one past it.
    EXPECT_EQ(labelProblem("vfat", "ABCDEFGHIJK"), "");
    EXPECT_NE(labelProblem("vfat", "ABCDEFGHIJKL"), "");
    EXPECT_EQ(labelProblem("exfat", std::string(11, 'a')), "");
    EXPECT_NE(labelProblem("exfat", std::string(12, 'a')), "");
    EXPECT_EQ(labelProblem("ntfs", std::string(32, 'a')), "");
    EXPECT_NE(labelProblem("ntfs", std::string(33, 'a')), "");
    EXPECT_EQ(labelProblem("ext4", std::string(16, 'a')), "");
    EXPECT_NE(labelProblem("ext4", std::string(17, 'a')), "");
    EXPECT_EQ(labelProblem("xfs", std::string(12, 'a')), "");
    EXPECT_NE(labelProblem("xfs", std::string(13, 'a')), "");
    EXPECT_EQ(labelProblem("btrfs", std::string(255, 'a')), "");
    EXPECT_NE(labelProblem("btrfs", std::string(256, 'a')), "");
    EXPECT_EQ(labelProblem("ext4", ""), "");
    // The older ext filesystems share ext4's field; swap keeps 15 bytes.
    for (const char* type : {"ext2", "ext3"}) {
        EXPECT_EQ(maximumLabelLength(type), 16u) << type;
        EXPECT_EQ(labelProblem(type, std::string(16, 'a')), "") << type;
        EXPECT_NE(labelProblem(type, std::string(17, 'a')), "") << type;
    }
    EXPECT_EQ(maximumLabelLength("swap"), 15u);
    EXPECT_EQ(labelProblem("swap", std::string(15, 'a')), "");
    EXPECT_NE(labelProblem("swap", std::string(16, 'a')).find("15 bytes"), std::string::npos);

    // Characters for FAT32, exFAT and NTFS, bytes for the others: "é" is
    // two bytes.
    const std::string accented = "\xc3\xa9";
    std::string eleven;
    for (int i = 0; i < 11; ++i)
        eleven += accented;
    EXPECT_EQ(labelProblem("exfat", eleven), "");
    EXPECT_EQ(labelProblem("ext4", eleven.substr(0, 16)), "");
    EXPECT_NE(labelProblem("ext4", eleven.substr(0, 18)), "");
    EXPECT_NE(labelProblem("ext4", std::string(17, 'a')).find("16 bytes"), std::string::npos);
    EXPECT_NE(labelProblem("ntfs", std::string(33, 'a')).find("32 characters"), std::string::npos);
}

TEST(VolumeFormatTest, LabelCharacters)
{
    EXPECT_NE(labelProblem("vfat", "A/B"), "");
    EXPECT_NE(labelProblem("vfat", "A.B"), "");
    EXPECT_NE(labelProblem("vfat", "A\"B").find("“\"”"), std::string::npos);
    EXPECT_NE(labelProblem("vfat", "\xc3\xa9t\xc3\xa9"), "");
    EXPECT_EQ(labelProblem("vfat", "MY DISK-1"), "");
    EXPECT_EQ(labelProblem("ext4", "a.b/c"), "");
    EXPECT_NE(labelProblem("ext4", "tab\there"), "");
}

TEST(VolumeFormatTest, FatNamesInCapitals)
{
    EXPECT_EQ(normalizedLabel("vfat", "Stick 2"), "STICK 2");
    EXPECT_EQ(normalizedLabel("exfat", "Stick 2"), "Stick 2");
    EXPECT_EQ(defaultLabel("vfat"), "UNTITLED");
    EXPECT_EQ(defaultLabel("ext4"), "Untitled");
    // A lowercase name is fine: it is made capitals.
    EXPECT_EQ(labelProblem("vfat", "stick"), "");
}

TEST(VolumeFormatTest, MinimumSizes)
{
    EXPECT_EQ(minimumSize("xfs"), 300 * kMiB);
    EXPECT_EQ(minimumSize("btrfs"), 128 * kMiB);
    EXPECT_GT(minimumSize("vfat"), 32 * kMiB);
    EXPECT_EQ(minimumSize("ext4"), 8 * kMiB);
}

TEST(VolumeFormatTest, Alignment)
{
    EXPECT_EQ(alignUp(1), kMiB);
    EXPECT_EQ(alignUp(kMiB), kMiB);
    EXPECT_EQ(alignDown(kMiB + 1), kMiB);
    EXPECT_EQ(partitionNameFor(std::string(40, 'a')), std::string(36, 'a'));
    // Never half a character.
    std::string accented;
    for (int i = 0; i < 40; ++i)
        accented += "\xc3\xa9";
    EXPECT_EQ(partitionNameFor(accented).size(), 72u);
}

TEST(VolumeFormatTest, AvailabilityFollowsCanFormat)
{
    properdesktop::base::EventLoop loop;
    Model model;
    loadSample(model);
    FakeBackend backend;
    backend.responder = [](const Call& call) -> std::optional<Result> {
        if (call.method != "CanFormat")
            return std::nullopt;
        const std::string type = call.arguments.at(0).value<std::string>();
        const bool missing = type == "xfs" || type == "btrfs";
        return Result{{}, {}, {Variant(std::tuple<bool, std::string>(!missing, missing ? "mkfs." + type : ""))}};
    };
    Operations operations(model, backend);
    FormatAvailability availability(operations);
    int changes = 0;
    auto subscription = availability.changed.connect([&] { ++changes; });
    EXPECT_EQ(availability.entry("ext4").state, FormatAvailability::State::Unknown);
    EXPECT_FALSE(availability.reason("ext4").empty());
    availability.query();
    ASSERT_EQ(backend.calls.size(), 6u);
    runPosted();
    EXPECT_EQ(changes, 6);
    EXPECT_TRUE(availability.isAvailable("ext4"));
    EXPECT_TRUE(availability.isAvailable("ntfs"));
    EXPECT_FALSE(availability.isAvailable("xfs"));
    EXPECT_EQ(availability.reason("ext4"), "");
    EXPECT_NE(availability.reason("xfs").find("mkfs.xfs"), std::string::npos);
}
