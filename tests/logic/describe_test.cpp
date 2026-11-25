// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "udisks/describe.h"
#include "udisks/model.h"

#include "base/locale.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace udisks;
using namespace udisks::fake;

static constexpr uint64_t kGB = 1000 * 1000 * 1000;

class DescribeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        loadSample(model);
        tree = buildTree(model, ViewMode::AllDevices);
    }

    const Node& node(const std::string& id)
    {
        const Node* found = findNode(tree, id);
        if (!found)
            throw std::runtime_error("no node " + id);
        return *found;
    }

    Model model;
    std::vector<SectionNodes> tree;
};

TEST_F(DescribeTest, Names)
{
    EXPECT_EQ(displayName(model, node("d:" + sample::kNvme)), "YMTC PC005 512GB");
    EXPECT_EQ(displayName(model, node("d:" + sample::kSata)), "Seagate ST1000LM035");
    EXPECT_EQ(displayName(model, node("d:" + sample::kImage)), "debian.iso");
    EXPECT_EQ(displayName(model, node("v:" + sample::kRoot)), "ProperDesktop");
    EXPECT_EQ(displayName(model, node("v:" + sample::kEfi)), "EFI");
    // The cleartext block's label, and the size while locked.
    EXPECT_EQ(displayName(model, node("v:" + sample::kUnlocked)), "Secret");
    EXPECT_EQ(displayName(model, node("v:" + sample::kLocked)), formatSize(100 * kGB) + " Encrypted Volume");
    EXPECT_EQ(displayName(model, node("p:" + sample::kUnformatted)), "sdc3");
    EXPECT_EQ(displayName(model, node("c:" + sample::kExtended)), "Extended Partition");
    EXPECT_EQ(displayName(model, node("d:" + sample::kCard)), "SD Card");
    EXPECT_EQ(deviceName(model, sample::kRoot), "nvme0n1p2");
}

TEST_F(DescribeTest, NamesWithoutLabels)
{
    model.changeProperties(sample::kData, interface::kBlock, {{"IdLabel", Variant(std::string())}});
    EXPECT_EQ(displayName(model, node("v:" + sample::kData)), formatSize(500 * kGB) + " Volume");
    model.changeProperties(sample::kSataDrive, interface::kDrive,
                           {{"Vendor", Variant(std::string())}, {"Model", Variant(std::string())}});
    EXPECT_EQ(displayName(model, node("d:" + sample::kSata)), formatSize(1000204886016) + " Disk");
}

TEST_F(DescribeTest, Types)
{
    EXPECT_EQ(filesystemName("vfat", "FAT32"), "FAT32");
    EXPECT_EQ(filesystemName("vfat"), "FAT");
    EXPECT_EQ(filesystemName("exfat"), "exFAT");
    EXPECT_EQ(filesystemName("ntfs"), "NTFS");
    EXPECT_EQ(filesystemName("ext4"), "ext4");
    EXPECT_EQ(filesystemName("crypto_LUKS"), "LUKS");
    EXPECT_EQ(filesystemName("iso9660"), "ISO 9660");
    EXPECT_EQ(filesystemName(""), "");
    EXPECT_EQ(schemeName("gpt"), "GUID Partition Map");
    EXPECT_EQ(schemeName("dos"), "Master Boot Record");
    EXPECT_EQ(schemeName(""), "");
    EXPECT_EQ(formatSize(512110190592), "512.11 GB");
}

TEST_F(DescribeTest, Connections)
{
    EXPECT_EQ(connectionOf(model, sample::kNvmeDrive), Connection::Nvme);
    EXPECT_EQ(connectionOf(model, sample::kSataDrive), Connection::Sata);
    EXPECT_EQ(connectionOf(model, sample::kStickDrive), Connection::Usb);
    EXPECT_EQ(connectionOf(model, sample::kCardDrive), Connection::SdCard);
    EXPECT_EQ(connectionOf(model, {}), Connection::DiskImage);
    EXPECT_EQ(connectionName(Connection::Usb), "USB");
    EXPECT_EQ(connectionName(Connection::Unknown), "");
}

TEST_F(DescribeTest, Icons)
{
    EXPECT_EQ(iconName(model, node("d:" + sample::kNvme)), "drive-harddisk");
    EXPECT_EQ(iconName(model, node("d:" + sample::kStick)), "drive-removable-media");
    EXPECT_EQ(iconName(model, node("d:" + sample::kCard)), "media-flash");
}

TEST_F(DescribeTest, MountPoints)
{
    EXPECT_EQ(mountPointOf(model, node("v:" + sample::kRoot)), "/");
    EXPECT_EQ(mountPointOf(model, node("v:" + sample::kUnlocked)), "/media/u/Secret");
    EXPECT_EQ(mountPointOf(model, node("v:" + sample::kLocked)), "");
    EXPECT_EQ(mountPointOf(model, node("d:" + sample::kNvme)), "");
}

TEST_F(DescribeTest, UsageOfAMountedVolume)
{
    std::string asked;
    const StatFunction stat = [&](const std::string& point) -> std::optional<FilesystemStats> {
        asked = point;
        // 100 GB in 4 KiB blocks, 60 GB free of which 5 GB reserved.
        const uint64_t block = 4096;
        return FilesystemStats{block, 100 * kGB / block, 60 * kGB / block, 55 * kGB / block};
    };
    const Usage usage = usageOf(model, node("v:" + sample::kUnlocked), stat);
    EXPECT_EQ(asked, "/media/u/Secret");
    EXPECT_TRUE(usage.known);
    EXPECT_EQ(usage.capacity, 200 * kGB);
    const uint64_t block = 4096;
    EXPECT_EQ(usage.used, (100 * kGB / block - 60 * kGB / block) * block);
    EXPECT_EQ(usage.available, 55 * kGB / block * block);
    EXPECT_EQ(usage.other(), usage.capacity - usage.used - usage.available);
}

TEST_F(DescribeTest, UsageClampedToTheCapacity)
{
    // A filesystem reporting more than its block holds.
    const StatFunction stat = [](const std::string&) -> std::optional<FilesystemStats> {
        return FilesystemStats{1, 900 * kGB, 100 * kGB, 100 * kGB};
    };
    const Usage usage = usageOf(model, node("v:" + sample::kUnlocked), stat);
    EXPECT_EQ(usage.used, 200 * kGB);
    EXPECT_EQ(usage.available, 0u);
    EXPECT_EQ(usage.other(), 0u);
}

TEST_F(DescribeTest, UsageUnknownWhileUnmounted)
{
    bool asked = false;
    const StatFunction stat = [&](const std::string&) -> std::optional<FilesystemStats> {
        asked = true;
        return std::nullopt;
    };
    const Usage usage = usageOf(model, node("v:" + sample::kData), stat);
    EXPECT_FALSE(asked);
    EXPECT_FALSE(usage.known);
    EXPECT_EQ(usage.capacity, 500 * kGB);

    // statvfs failing on a mounted one.
    EXPECT_FALSE(usageOf(model, node("v:" + sample::kRoot), stat).known);
}

TEST_F(DescribeTest, UsageOfADisk)
{
    const Usage sata = usageOf(model, node("d:" + sample::kSata));
    EXPECT_TRUE(sata.known);
    EXPECT_EQ(sata.capacity, 1000204886016u);
    EXPECT_EQ(sata.used, 500 * kGB);
    EXPECT_EQ(sata.available, 1000204886016u - 500 * kGB);

    // Logical partitions are inside the extended one, counted once.
    const Usage mbr = usageOf(model, node("d:" + sample::kMbr));
    EXPECT_EQ(mbr.used, 300 * kGB);

    const Usage card = usageOf(model, node("d:" + sample::kCard));
    EXPECT_FALSE(card.known);
    EXPECT_EQ(card.capacity, 31914983424u);
}

static constexpr uint64_t kMB = 1000 * 1000;

using properdesktop::base::Locale;

TEST(SizeTest, NumbersInTheUnit)
{
    const Locale english("en_US");
    EXPECT_EQ(parseSize("200", kGB, english), 200 * kGB);
    EXPECT_EQ(parseSize("100", kMB, english), 100 * kMB);
    EXPECT_EQ(parseSize(" 1.5 ", kGB, english), 1'500'000'000u);
    EXPECT_EQ(parseSize(".5", kGB, english), 500'000'000u);
    EXPECT_EQ(parseSize("1.", kMB, english), 1'000'000u);
}

TEST(SizeTest, OnlyTheLocalesDecimalSeparator)
{
    const Locale german("de_DE");
    EXPECT_EQ(parseSize("1,5", kGB, german), 1'500'000'000u);
    EXPECT_EQ(parseSize("2,25 TB", kMB, german), 2'250'000'000'000u);
    // A thousands separator is no decimal point, and is not guessed at.
    EXPECT_FALSE(parseSize("1.500", kGB, german));
    EXPECT_FALSE(parseSize("1.5", kGB, german));

    const Locale english("en_US");
    EXPECT_FALSE(parseSize("1,5", kGB, english));
    EXPECT_FALSE(parseSize("1,500", kGB, english));
    EXPECT_FALSE(parseSize("1,5.2", kGB, english));

    const Locale french("fr_FR");
    EXPECT_EQ(parseSize("1,5", kGB, french), 1'500'000'000u);
    EXPECT_FALSE(parseSize("1.5", kGB, french));
}

// What formatSize() shows reads back as the size it stands for, to its two
// decimals.
TEST(SizeTest, TheShownTextReadsBack)
{
    for (const char* name : {"en_US", "de_DE", "fr_FR", "ru_RU"}) {
        const Locale locale(name);
        for (uint64_t bytes :
             {uint64_t(512110190592), uint64_t(1500) * kMB, uint64_t(33) * kMB, uint64_t(2000) * kGB, uint64_t(999)}) {
            const std::string shown = formatSize(bytes, locale);
            const std::optional<uint64_t> read = parseSize(shown, kGB, locale);
            ASSERT_TRUE(read) << name << ": " << shown;
            uint64_t unit = 1;
            while (unit * 1000 <= bytes)
                unit *= 1000;
            const double error = std::abs(double(*read) - double(bytes));
            EXPECT_LE(error, unit * 0.005 + 0.5) << name << ": " << shown;
        }
    }
}

// A size that rounds up to a thousand of its unit shows as one of the next,
// without a group separator, and reads back.
TEST(SizeTest, TheRoundingEdgeShowsTheNextUnit)
{
    EXPECT_EQ(formatSize(999'995'000'000, Locale("en_US")), "1.00 TB");
    EXPECT_EQ(formatSize(999'994'999'999, Locale("en_US")), "999.99 GB");
    EXPECT_EQ(formatSize(999'999, Locale("en_US")), "1.00 MB");
    for (const char* name : {"en_US", "de_DE", "fr_FR"}) {
        const Locale locale(name);
        for (uint64_t bytes : {uint64_t(999'994'999'999), uint64_t(999'995'000'000), uint64_t(999'999'999'999),
                               uint64_t(1'000'000'000'000), uint64_t(999'995), uint64_t(999'999'999)}) {
            const std::string shown = formatSize(bytes, locale);
            const std::optional<uint64_t> read = parseSize(shown, kGB, locale);
            ASSERT_TRUE(read) << name << ": " << shown;
            EXPECT_LE(std::abs(double(*read) - double(bytes)), double(bytes) * 0.0001) << name << ": " << shown;
        }
    }
}

TEST(SizeTest, SuffixesOverrideTheUnit)
{
    const Locale english("en_US");
    EXPECT_EQ(parseSize("200 GB", kGB, english), 200 * kGB);
    EXPECT_EQ(parseSize(" 1.5tb ", kGB, english), 1'500'000'000'000u);
    EXPECT_EQ(parseSize("512 MB", kGB, english), 512 * kMB);
    EXPECT_EQ(parseSize("512M", kGB, english), 512 * kMB);
    EXPECT_EQ(parseSize("1.5G", kMB, english), 1'500'000'000u);
    EXPECT_EQ(parseSize("2 GiB", kMB, english), 2ull << 30);
    EXPECT_EQ(parseSize("4096 b", kGB, english), 4096u);
    EXPECT_EQ(parseSize("3k", kGB, english), 3000u);
    EXPECT_EQ(parseSize("1 tib", kMB, english), 1ull << 40);
    // The ASCII units in every locale.
    EXPECT_EQ(parseSize("2 GB", kMB, Locale("fr_FR")), 2 * kGB);
    EXPECT_EQ(parseSize("2 GB", kMB, Locale("ru_RU")), 2 * kGB);
}

TEST(SizeTest, RejectsWhatIsNoSize)
{
    const Locale english("en_US");
    for (const char* text : {"", "  ", "abc", "big", "MB", "-1", "-1 GB", "0", "0.0000", "1.2.3", "5 XB", "2 PB",
                             "1 MBB", "1e3"})
        EXPECT_FALSE(parseSize(text, kMB, english)) << text;
    EXPECT_FALSE(parseSize("99999999999999999999", kMB, english));
    EXPECT_FALSE(parseSize("10000000 TB", kMB, english));
}
