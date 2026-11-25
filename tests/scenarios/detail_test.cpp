// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "udisks/model.h"
#include "udisks/smart.h"
#include "views/detailcontent.h"

#include <gtest/gtest.h>

using namespace properdesktop::ui;
using namespace udisks::fake;

static std::string row(const std::string& id)
{
    return "sidebar:" + id;
}

TEST(DetailTest, MountedVolume)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(row("v:" + sample::kRoot));
    EXPECT_EQ(driver.text("detail:name"), "ProperDesktop");
    EXPECT_EQ(driver.text("detail:mountPoint"), "/");
    EXPECT_EQ(driver.text("detail:type"), "ext4");
    EXPECT_EQ(driver.text("detail:device"), "nvme0n1p2");
    EXPECT_EQ(driver.text("detail:connection"), "NVMe");
    EXPECT_EQ(driver.text("detail:uuid"), "1c8f081f-c3e2-4dc2-823a-fa455249cbb6");
    EXPECT_NE(driver.text("detail:subtitle").find("ext4"), std::string::npos);
    EXPECT_TRUE(driver.shows("detail:summary"));
    EXPECT_TRUE(driver.shows("detail:usage"));
    EXPECT_TRUE(driver.shows("detail:used"));
    EXPECT_TRUE(driver.shows("detail:available"));
    EXPECT_EQ(driver.countShown("detail:partitionMap"), 0u);
    EXPECT_EQ(driver.countShown("detail:empty"), 0u);
}

TEST(DetailTest, UnmountedVolumeAndItsMount)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(row("v:" + sample::kData));
    EXPECT_EQ(driver.text("detail:mountPoint"), "Not Mounted");
    EXPECT_EQ(driver.text("detail:type"), "NTFS");
    // Nothing is known of its space.
    EXPECT_EQ(driver.countShown("detail:usage"), 0u);
    EXPECT_EQ(driver.countShown("detail:used"), 0u);

    std::vector<std::vector<uint8_t>> points{udisks::encodeBytes("/tmp")};
    scenario.model().changeProperties(sample::kData, udisks::interface::kFilesystem,
                                      {{"MountPoints", udisks::Variant(points)}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.text("detail:mountPoint") == "/tmp";
    }));
    EXPECT_TRUE(driver.shows("detail:usage"));
    EXPECT_TRUE(driver.shows("detail:used"));
}

TEST(DetailTest, DiskWithPartitions)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    driver.layout();
    driver.click(row("d:" + sample::kSata));
    EXPECT_EQ(driver.text("detail:name"), "Seagate ST1000LM035");
    EXPECT_EQ(driver.text("detail:partitionMap"), "GUID Partition Map");
    EXPECT_EQ(driver.text("detail:children"), "1");
    EXPECT_EQ(driver.text("detail:location"), "Internal");
    EXPECT_EQ(driver.text("detail:connection"), "SATA");
    EXPECT_EQ(driver.text("detail:device"), "sda");
    EXPECT_EQ(driver.text("detail:smart"),
              udisks::smartStatusName(udisks::smartStatus(scenario.model(), sample::kSataDrive)));
    EXPECT_EQ(driver.countShown("detail:mountPoint"), 0u);
    // The partition and the free space after it.
    EXPECT_TRUE(driver.shows("detail:usage"));
    EXPECT_TRUE(driver.shows("detail:legend:0"));
    EXPECT_TRUE(driver.shows("detail:legend:1"));
    EXPECT_EQ(driver.countShown("detail:legend:2"), 0u);

    const views::DetailContent content = views::describeDetail(scenario.model(), *scenario.window->actions().selection());
    ASSERT_EQ(content.segments.size(), 2u);
    EXPECT_EQ(content.segments[0].kind, views::UsageSegment::Kind::Partition);
    EXPECT_EQ(content.segments[0].title, "Data");
    EXPECT_EQ(content.segments[1].kind, views::UsageSegment::Kind::Free);
}

TEST(DetailTest, ExtendedPartitionShowsItsLogicalOnes)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    driver.layout();
    driver.click(row("c:" + sample::kExtended));
    EXPECT_EQ(driver.text("detail:type"), "Extended Partition");
    const views::DetailContent content = views::describeDetail(scenario.model(), *scenario.window->actions().selection());
    ASSERT_GE(content.segments.size(), 2u);
    EXPECT_EQ(content.segments[0].title, "Shared");
}

TEST(DetailTest, JobProgress)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(row("v:" + sample::kData));
    EXPECT_EQ(driver.countShown("detail:job"), 0u);

    TreeBuilder jobs;
    const std::string job = jobs.addJob("7", "format-mkfs", {sample::kData}, 0.4);
    scenario.model().addInterfaces(job, jobs.objects().at(job));
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.countShown("detail:job") == 1;
    }));
    EXPECT_EQ(driver.text("detail:job"), "Erasing…");
    EXPECT_TRUE(driver.shows("detail:jobProgress"));
    EXPECT_TRUE(driver.showsText("40%"));

    scenario.model().removeInterfaces(job, {udisks::interface::kJob});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.countShown("detail:job") == 0;
    }));
}

TEST(DetailTest, EmptyWithoutDisks)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    EXPECT_EQ(driver.countShown("detail:empty"), 0u);
    scenario.model().reset({});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.shows("detail:empty");
    }));
    EXPECT_EQ(driver.countShown("detail:name"), 0u);
    EXPECT_TRUE(driver.showsText("No Disks"));
}
