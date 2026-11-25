// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "dialogs/choice_button.h"
#include "dialogs/partition.h"
#include "dialogs/pie_chart.h"
#include "dialogs/volume_format.h"
#include "udisks/describe.h"
#include "udisks/model.h"
#include "views/context.h"

#include "ui/keys.h"
#include "ui/test_input.h"
#include "ui/text_field.h"

#include <gtest/gtest.h>

#include <cmath>
#include <tuple>

#include <sys/sysmacros.h>

using namespace properdesktop::ui;
using namespace udisks::fake;
using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

static constexpr uint64_t kGB = 1000 * 1000 * 1000;

// The partition editor over the sample disks; every filesystem's tools are
// there and NTFS resizes offline.
class PartitionTest : public testing::Test
{
protected:
    void SetUp() override
    {
        scenario.backend().responder = [this](const udisks::Call& call) -> std::optional<udisks::Result> {
            if (call.method == "CanFormat")
                return udisks::Result{{}, {}, {Variant(std::tuple<bool, std::string>(true, ""))}};
            if (call.method == "CanResize")
                return udisks::Result{{}, {}, {Variant(std::tuple<bool, uint64_t, std::string>(true, 2 | 4, ""))}};
            if (call.method == failing)
                return error("org.freedesktop.UDisks2.Error.Failed", "Resize failed");
            return udisks::Result{};
        };
    }

    std::vector<std::string> calls() const
    {
        std::vector<std::string> result;
        for (const udisks::Call& call : const_cast<PartitionTest*>(this)->scenario.backend().calls) {
            if (!call.method.starts_with("Can"))
                result.push_back(call.path + " " + call.method);
        }
        return result;
    }

    std::unique_ptr<TestDriver> open(const std::string& id)
    {
        scenario.window->select(id);
        scenario.driver.layout();
        scenario.driver.click("toolbar:partition");
        scenario.driver.layout();
        Window* window = scenario.driver.dialog();
        if (!window) {
            ADD_FAILURE() << "no dialog open";
            return nullptr;
        }
        auto driver = std::make_unique<TestDriver>(*window, [](const std::string& message) { ADD_FAILURE() << message; });
        driver->setKeyboardFocus(true);
        runPosted();
        driver->layout();
        return driver;
    }

    static void replaceText(TestDriver& driver, const std::string& name, const std::string& text)
    {
        driver.click(name);
        driver.typeKey(key::letter('a'), ControlModifier);
        driver.typeKey(key::BackSpace);
        driver.typeText(text);
    }

    static bool enabled(TestDriver& driver, const std::string& name)
    {
        const View* view = driver.find(name);
        return view && view->isEnabled();
    }

    bool runs(TestDriver& driver)
    {
        return TestDriver::waitFor([&] {
            driver.layout();
            return driver.shows("partition:done");
        });
    }

    Scenario scenario;
    std::string failing;
};

TEST_F(PartitionTest, AddsAPartition)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    EXPECT_TRUE(driver->shows("partition:pie"));
    // The volume comes selected; nothing to apply yet.
    EXPECT_EQ(driver->text("partition:name"), "Data");
    EXPECT_FALSE(enabled(*driver, "partition:apply"));

    driver->click("partition:add");
    EXPECT_EQ(driver->text("partition:name"), "Untitled");
    EXPECT_EQ(driver->find<dialogs::ChoiceButton>("partition:format")->currentValue(), "ext4");
    replaceText(*driver, "partition:name", "Music");
    driver->click("partition:format");
    EXPECT_TRUE(driver->choosePopupItem("exFAT"));
    ASSERT_TRUE(enabled(*driver, "partition:apply"));

    driver->click("partition:apply");
    // The summary first; nothing has been sent.
    EXPECT_TRUE(driver->shows("partition:summary:0"));
    EXPECT_NE(driver->texts("partition:summary:0").front().find("Create “Music”, exFAT"), std::string::npos);
    EXPECT_TRUE(calls().empty());
    driver->click("partition:confirm");
    ASSERT_TRUE(runs(*driver));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kSata + " CreatePartitionAndFormat"});
    EXPECT_NE(driver->text("partition:status").find("was partitioned"), std::string::npos);
}

TEST_F(PartitionTest, ResizesWithTheSizeField)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    replaceText(*driver, "partition:size", "250");
    driver->typeKey(key::Return);
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(250 * kGB));
    driver->click("partition:apply");
    driver->click("partition:confirm");
    ASSERT_TRUE(runs(*driver));
    EXPECT_EQ(calls(), (std::vector<std::string>{sample::kData + " Resize", sample::kData + " Resize"}));
    const auto& all = scenario.backend().calls;
    std::vector<std::string> interfaces;
    for (const udisks::Call& call : all) {
        if (call.method == "Resize")
            interfaces.push_back(call.interface);
    }
    // The filesystem shrinks before its partition.
    EXPECT_EQ(interfaces, (std::vector<std::string>{udisks::interface::kFilesystem, udisks::interface::kPartition}));
}

TEST_F(PartitionTest, DragsABoundary)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    auto* pie = driver->find<dialogs::PieChart>("partition:pie");
    ASSERT_TRUE(pie);
    ASSERT_TRUE(pie->handlePoint(0));
    ASSERT_TRUE(pie->onBoundaryMoved);
    // The handle dragged to three quarters of the disk.
    pie->onBoundaryMoved(0, dialogs::kMiB + 750 * kGB);
    pie->onDragFinished();
    driver->layout();
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(750 * kGB));
    EXPECT_TRUE(enabled(*driver, "partition:apply"));
    EXPECT_TRUE(enabled(*driver, "partition:revert"));
    driver->click("partition:revert");
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(500 * kGB));
    EXPECT_FALSE(enabled(*driver, "partition:apply"));
}

TEST_F(PartitionTest, StopsAtTheFirstFailure)
{
    failing = "Resize";
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    replaceText(*driver, "partition:size", "250 GB");
    driver->typeKey(key::Return);
    driver->click("partition:add");
    driver->click("partition:apply");
    driver->click("partition:confirm");
    ASSERT_TRUE(runs(*driver));
    // The create after the failed shrink never ran.
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kData + " Resize"});
    EXPECT_NE(driver->text("partition:error").find("Resize failed"), std::string::npos);
    EXPECT_NE(driver->text("partition:status").find("Nothing was changed"), std::string::npos);
}

TEST_F(PartitionTest, DeletesAndRecreates)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    driver->click("partition:remove");
    EXPECT_FALSE(driver->find<View>("partition:name")->isEnabled());
    driver->click("partition:add");
    driver->click("partition:apply");
    driver->click("partition:confirm");
    ASSERT_TRUE(runs(*driver));
    EXPECT_EQ(calls(), (std::vector<std::string>{sample::kData + " Delete", sample::kSata + " CreatePartitionAndFormat"}));
    const udisks::Call& create = scenario.backend().calls.back();
    EXPECT_EQ(create.arguments.at(0).value<uint64_t>(), dialogs::kMiB);
}

TEST_F(PartitionTest, ATypedSizeStaysWithItsSlice)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    auto* pie = driver->find<dialogs::PieChart>("partition:pie");
    ASSERT_TRUE(pie);
    // The editor numbers the slices in disk order: Data, then the free space.
    const dialogs::PartitionLayout layout = dialogs::PartitionLayout::fromDisk(scenario.model(), sample::kSata);
    const uint32_t data = layout.slices()[0].id;
    const uint32_t free = layout.slices()[1].id;

    // Into the size field and out again: nothing changes.
    driver->click("partition:size");
    driver->click("partition:name");
    EXPECT_FALSE(enabled(*driver, "partition:apply"));

    // Typed for Data, then the free space clicked: Data takes it.
    replaceText(*driver, "partition:size", "250");
    pie->onSelected(free);
    driver->layout();
    pie->onSelected(data);
    driver->layout();
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(250 * kGB));
    EXPECT_TRUE(enabled(*driver, "partition:apply"));
}

TEST_F(PartitionTest, PlusTakesTheTypedSizeFirst)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    auto* pie = driver->find<dialogs::PieChart>("partition:pie");
    ASSERT_TRUE(pie);
    const uint32_t data = dialogs::PartitionLayout::fromDisk(scenario.model(), sample::kSata).slices()[0].id;

    // Typed for Data, then + (which takes no focus): Data takes the size
    // before the new partition is made, and keeps it after.
    replaceText(*driver, "partition:size", "250");
    driver->click("partition:add");
    EXPECT_EQ(driver->text("partition:name"), "Untitled");
    const std::string created = driver->text("partition:size");
    driver->click("partition:name");
    EXPECT_EQ(driver->text("partition:size"), created);
    pie->onSelected(data);
    driver->layout();
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(250 * kGB));
}

TEST_F(PartitionTest, RevertDropsTheTypedSize)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    driver->click("partition:add");
    replaceText(*driver, "partition:size", "30");
    driver->click("partition:revert");
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(500 * kGB));
    driver->click("partition:name");
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(500 * kGB));
    EXPECT_FALSE(enabled(*driver, "partition:apply"));
}

TEST_F(PartitionTest, AFailedNewMapSaysTheDiskChanged)
{
    failing = "Format";
    auto driver = open("d:" + sample::kCard);
    ASSERT_TRUE(driver);
    driver->click("partition:add");
    driver->click("partition:apply");
    driver->click("partition:confirm");
    ASSERT_TRUE(runs(*driver));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kCard + " Format"});
    EXPECT_NE(driver->text("partition:status").find("only in part"), std::string::npos);
}

TEST_F(PartitionTest, FatNamesInCapitals)
{
    auto driver = open("v:" + sample::kData);
    ASSERT_TRUE(driver);
    driver->click("partition:add");
    driver->click("partition:format");
    EXPECT_TRUE(driver->choosePopupItem("FAT32 (MS-DOS)"));
    EXPECT_EQ(driver->text("partition:name"), "UNTITLED");
    replaceText(*driver, "partition:name", "photos");
    EXPECT_EQ(driver->text("partition:name"), "PHOTOS");
}

TEST_F(PartitionTest, MountedVolumesNeedUnmounting)
{
    auto driver = open("v:" + sample::kStickVolume);
    ASSERT_TRUE(driver);
    EXPECT_FALSE(enabled(*driver, "partition:size"));
    EXPECT_NE(driver->text("partition:note").find("Unmount “STICK”"), std::string::npos);
}

TEST_F(PartitionTest, MasterBootRecordLimit)
{
    auto driver = open("v:" + sample::kMbrLinux);
    ASSERT_TRUE(driver);
    driver->click("partition:add");
    driver->click("partition:add");
    EXPECT_FALSE(enabled(*driver, "partition:add"));
    EXPECT_EQ(driver->find<View>("partition:add")->toolTip(), "A Master Boot Record holds at most four partitions.");
}

TEST_F(PartitionTest, ADragAfterATypedSizeTakesTheRightHandle)
{
    // A disk of two ext4 partitions, A and B, and free space after them.
    TreeBuilder tree;
    const uint64_t size = 100 * kGB;
    const std::string drive = tree.addDrive("Spare", {.model = "Spare", .size = size, .sortKey = "02"});
    const std::string disk = tree.addDisk("sdx", drive, {.size = size, .deviceNumber = makedev(8, 96)}, "gpt");
    std::vector<std::string> parts;
    for (int i = 0; i < 2; ++i) {
        parts.push_back(tree.addPartition(disk, "sdx" + std::to_string(i + 1), uint32_t(i + 1),
                                          dialogs::kMiB + uint64_t(i) * 40 * kGB,
                                          {.size = 40 * kGB, .deviceNumber = makedev(8, 97 + i), .idUsage = "filesystem",
                                           .idType = "ext4", .idLabel = i == 0 ? "A" : "B"}));
        tree.setFilesystem(parts.back(), {});
    }
    scenario.model().reset(tree.objects());
    ASSERT_TRUE(TestDriver::waitFor([&] {
        scenario.driver.layout();
        return scenario.driver.countShown("sidebar:v:" + parts[0]) == 1;
    }));
    auto driver = open("v:" + parts[0]);
    ASSERT_TRUE(driver);
    auto* pie = driver->find<dialogs::PieChart>("partition:pie");
    ASSERT_TRUE(pie);
    const auto ids = dialogs::PartitionLayout::fromDisk(scenario.model(), disk).slices();
    ASSERT_EQ(ids.size(), 3u);

    // A typed smaller, then the B | free space handle dragged without
    // leaving the field first: shrinking A adds a wedge before B.
    replaceText(*driver, "partition:size", "20");
    const std::optional<PointF> handle = pie->handlePoint(1);
    ASSERT_TRUE(handle);
    const RectF frame = driver->frame("partition:pie");
    const double radius = std::min(frame.width, frame.height) / 2 - 10;
    const PointF centre{frame.x + frame.width / 2, frame.y + frame.height / 2};
    // Seven eighths of the way round: about 87.5 GB into the disk.
    const double angle = 2 * M_PI * 0.875;
    const PointF target{centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
    TestInput::press(driver->window(), {frame.x + handle->x, frame.y + handle->y});
    TestInput::movePointer(driver->window(), target);
    TestInput::release(driver->window(), target);
    driver->layout();

    pie->onSelected(ids[0].id);
    driver->layout();
    EXPECT_EQ(driver->text("partition:size"), udisks::formatSize(20 * kGB));
    pie->onSelected(ids[1].id);
    driver->layout();
    const std::optional<uint64_t> grown = udisks::parseSize(driver->text("partition:size"), kGB);
    ASSERT_TRUE(grown);
    EXPECT_GT(*grown, 45 * kGB);
    EXPECT_LT(*grown, 50 * kGB);
}
