// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "udisks/model.h"
#include "udisks/operations.h"

#include "ui/menu.h"

#include <gtest/gtest.h>

#include <sys/sysmacros.h>

using namespace properdesktop::ui;
using namespace udisks::fake;
using properdesktop::base::dbus::Variant;

static constexpr uint64_t kMiB = 1024 * 1024;

class SafetyTest : public testing::Test
{
protected:
    void select(const std::string& id)
    {
        scenario.window->select(id);
        scenario.driver.layout();
    }

    std::unique_ptr<TestDriver> over(TestDriver& driver)
    {
        Window* window = driver.dialog();
        if (!window)
            return nullptr;
        auto dialog = std::make_unique<TestDriver>(*window, [](const std::string& message) { ADD_FAILURE() << message; });
        dialog->layout();
        return dialog;
    }

    // The stick pulled out and another put in, at the same paths.
    void replugAnotherStick()
    {
        udisks::Model& model = scenario.model();
        model.removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock, udisks::interface::kPartition,
                                                      udisks::interface::kFilesystem});
        model.removeInterfaces(sample::kStick, {udisks::interface::kBlock, udisks::interface::kPartitionTable});
        model.removeInterfaces(sample::kStickDrive, {udisks::interface::kDrive});
        TreeBuilder other;
        const std::string drive = other.addDrive("SanDisk_Cruzer",
                                                 {.model = "Other", .serial = "9999", .size = 8000000000,
                                                  .connectionBus = "usb", .removable = true, .ejectable = true});
        const std::string disk = other.addDisk("sdb", drive, {.size = 8000000000, .deviceNumber = makedev(8, 16)},
                                               "dos");
        const std::string volume = other.addPartition(disk, "sdb1", 1, kMiB,
                                                      {.size = 8000000000 - kMiB, .deviceNumber = makedev(8, 17),
                                                       .idUsage = "filesystem", .idType = "vfat",
                                                       .idLabel = "PHOTOS"},
                                                      "0x0c");
        other.setFilesystem(volume, {});
        for (const std::string& path : {drive, disk, volume})
            model.addInterfaces(path, other.objects().at(path));
        runPosted();
        scenario.driver.layout();
    }

    std::vector<std::string> methods()
    {
        std::vector<std::string> names;
        for (const udisks::Call& call : scenario.backend().calls)
            names.push_back(call.method);
        return names;
    }

    Scenario scenario;
};

TEST_F(SafetyTest, EraseClosesWhenAnotherStickTakesThePath)
{
    scenario.backend().responder = [](const udisks::Call& call) -> std::optional<udisks::Result> {
        if (call.method == "CanFormat")
            return udisks::Result{{}, {}, {Variant(std::tuple<bool, std::string>(true, ""))}};
        return std::nullopt;
    };
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(scenario.window->actions().trigger(Action::Erase));
    ASSERT_TRUE(over(scenario.driver));
    replugAnotherStick();
    std::unique_ptr<TestDriver> alert = over(scenario.driver);
    ASSERT_TRUE(alert);
    EXPECT_TRUE(alert->showsText("“STICK” is no longer there."));
    for (const std::string& method : methods())
        EXPECT_NE(method, "Format");
}

TEST_F(SafetyTest, DeleteConfirmationGoesWithItsPartition)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(scenario.window->actions().trigger(Action::RemoveVolume));
    std::unique_ptr<TestDriver> confirm = over(scenario.driver);
    ASSERT_TRUE(confirm);
    EXPECT_TRUE(confirm->showsText("Delete “STICK”?"));
    replugAnotherStick();
    std::unique_ptr<TestDriver> alert = over(scenario.driver);
    ASSERT_TRUE(alert);
    EXPECT_TRUE(alert->showsText("“STICK” is no longer there."));
    EXPECT_FALSE(alert->showsText("Delete “STICK”?"));
    EXPECT_TRUE(scenario.backend().calls.empty());
}

TEST_F(SafetyTest, DeletingAnExtendedPartitionNamesWhatIsInIt)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("c:" + sample::kExtended);
    ASSERT_TRUE(scenario.window->actions().trigger(Action::RemoveVolume));
    std::unique_ptr<TestDriver> confirm = over(scenario.driver);
    ASSERT_TRUE(confirm);
    EXPECT_TRUE(confirm->showsText("The partitions in it, “Shared”, “sde6”, and everything stored on them are "
                                   "deleted. This can't be undone."));
}

TEST_F(SafetyTest, TheContextMenuActsOnItsRow)
{
    TestDriver& driver = scenario.driver;
    std::vector<std::string> erased;
    scenario.window->actions().setHandler(Action::Erase,
                                          [&](const Selection& selection) { erased.push_back(selection->block); });
    ASSERT_TRUE(driver.click("sidebar:v:" + sample::kStickVolume, MouseButton::Right));
    // Something else is selected before the item is chosen.
    scenario.window->select("v:" + sample::kData);
    EXPECT_TRUE(driver.choosePopupItem("Erase…"));
    EXPECT_EQ(erased, std::vector<std::string>{sample::kStickVolume});

    // Another stick at the row's path: nothing runs.
    ASSERT_TRUE(driver.click("sidebar:v:" + sample::kStickVolume, MouseButton::Right));
    replugAnotherStick();
    driver.choosePopupItem("Erase…");
    EXPECT_EQ(erased.size(), 1u);
}

TEST_F(SafetyTest, QuittingWhileBusyAsks)
{
    udisks::Operations::Hold hold = scenario.application.operations().hold(sample::kStick);
    scenario.window->requestQuit();
    std::unique_ptr<TestDriver> confirm = over(scenario.driver);
    ASSERT_TRUE(confirm);
    EXPECT_TRUE(confirm->showsText("An operation is in progress."));
    EXPECT_TRUE(confirm->clickText("Keep Running"));
    runPosted();
    // Still running: the window is there and the question gone.
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}
