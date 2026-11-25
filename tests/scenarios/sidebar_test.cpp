// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "udisks/model.h"
#include "udisks/operations.h"
#include "views/context.h"
#include "views/sidebar.h"

#include "ui/keys.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace properdesktop::ui;
using namespace udisks::fake;

static std::string row(const std::string& id)
{
    return "sidebar:" + id;
}

static std::string volume(const std::string& block)
{
    return row("v:" + block);
}

static std::string selectedId(Scenario& scenario)
{
    const Selection& selection = scenario.window->actions().selection();
    return selection ? selection->id : std::string();
}

static bool waitForSelection(Scenario& scenario, const std::string& id)
{
    return TestDriver::waitFor([&] {
        scenario.driver.layout();
        return selectedId(scenario) == id;
    });
}

TEST(SidebarTest, ArrowKeysMoveTheSelection)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(volume(sample::kRoot));
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kRoot);

    driver.typeKey(key::Down);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kData);
    EXPECT_EQ(driver.text("detail:name"), "Data");
    driver.typeKey(key::Up);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kRoot);
    // The first row stays where it is.
    driver.typeKey(key::Up);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kRoot);
    // Down crosses into the next section.
    driver.typeKey(key::Down);
    driver.typeKey(key::Down);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kStickVolume);
}

TEST(SidebarTest, ViewMenuSwitchesToAllDevices)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    EXPECT_EQ(driver.countShown("sidebar:disclosure:d:" + sample::kNvme), 0u);

    ASSERT_TRUE(driver.click("toolbar:view"));
    ASSERT_TRUE(driver.choosePopupItem("Show All Devices"));
    EXPECT_EQ(scenario.window->context().viewMode, udisks::ViewMode::AllDevices);
    EXPECT_TRUE(driver.shows(row("d:" + sample::kNvme)));
    EXPECT_TRUE(driver.shows("sidebar:disclosure:d:" + sample::kNvme));
    EXPECT_TRUE(driver.shows(volume(sample::kEfi)));
    // The selection stays on its volume.
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kRoot);
    // A disk with no partitions has nothing to fold.
    EXPECT_EQ(driver.countShown("sidebar:disclosure:d:" + sample::kCard), 0u);

    ASSERT_TRUE(driver.click("toolbar:view"));
    ASSERT_TRUE(driver.choosePopupItem("Show Only Volumes"));
    EXPECT_EQ(scenario.window->context().viewMode, udisks::ViewMode::Volumes);
    EXPECT_EQ(driver.countShown(row("d:" + sample::kNvme)), 0u);
}

TEST(SidebarTest, LeftAndRightFoldADisk)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    driver.layout();

    driver.click(row("d:" + sample::kMbr));
    driver.typeKey(key::Left);
    EXPECT_EQ(driver.countShown(volume(sample::kMbrLinux)), 0u);
    EXPECT_EQ(driver.countShown(volume(sample::kLogicalFat)), 0u);
    driver.typeKey(key::Right);
    EXPECT_TRUE(driver.shows(volume(sample::kMbrLinux)));
    EXPECT_TRUE(driver.shows(volume(sample::kLogicalFat)));
    // Right again goes into the disk, Left back out to it.
    driver.typeKey(key::Right);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kMbrLinux);
    driver.typeKey(key::Left);
    EXPECT_EQ(selectedId(scenario), "d:" + sample::kMbr);

    // Folding the disk of the selection selects the disk.
    driver.click(volume(sample::kLogicalFat));
    ASSERT_TRUE(driver.click("sidebar:disclosure:d:" + sample::kMbr));
    EXPECT_EQ(driver.countShown(volume(sample::kLogicalFat)), 0u);
    EXPECT_EQ(selectedId(scenario), "d:" + sample::kMbr);
    // The folding outlives a change of the disks.
    scenario.model().changeProperties(sample::kStickVolume, udisks::interface::kBlock,
                                      {{"IdLabel", udisks::Variant(std::string("Pocket"))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.showsText("Pocket");
    }));
    EXPECT_EQ(driver.countShown(volume(sample::kMbrLinux)), 0u);
    ASSERT_TRUE(driver.click("sidebar:disclosure:d:" + sample::kMbr));
    EXPECT_TRUE(driver.shows(volume(sample::kMbrLinux)));
}

TEST(SidebarTest, ContextMenuFollowsTheActions)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    ActionDispatcher& actions = scenario.window->actions();
    std::vector<std::string> erased;
    actions.setHandler(Action::Erase, [&](const Selection& selection) { erased.push_back(selection->block); });
    actions.setHandler(Action::Info, [](const Selection&) {});
    const auto* sidebar = driver.find<views::Sidebar>("sidebar");
    ASSERT_NE(sidebar, nullptr);
    const auto enabled = [sidebar](Action action) {
        const auto& entries = sidebar->menuEntries();
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [action](const views::Sidebar::MenuEntry& entry) { return entry.action == action; });
        return it != entries.end() && it->enabled;
    };
    const auto listed = [sidebar](Action action) {
        const auto& entries = sidebar->menuEntries();
        return std::any_of(entries.begin(), entries.end(),
                           [action](const views::Sidebar::MenuEntry& entry) { return entry.action == action; });
    };

    // The system volume: a right click selects it; Erase is there, greyed.
    ASSERT_TRUE(driver.click(volume(sample::kRoot), MouseButton::Right));
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kRoot);
    EXPECT_TRUE(enabled(Action::Info));
    EXPECT_TRUE(listed(Action::Erase));
    EXPECT_FALSE(enabled(Action::Erase));
    EXPECT_TRUE(listed(Action::Eject));
    EXPECT_FALSE(enabled(Action::Eject));
    driver.choosePopupItem(actionTitle(Action::Erase));
    EXPECT_TRUE(erased.empty());
    driver.typeKey(key::Escape);

    // Another disk's volume can go.
    ASSERT_TRUE(driver.click(volume(sample::kData), MouseButton::Right));
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kData);
    EXPECT_TRUE(enabled(Action::Erase));
    // Not mounted: the menu offers Mount.
    EXPECT_TRUE(listed(Action::Mount));
    EXPECT_FALSE(listed(Action::Unmount));
    ASSERT_TRUE(driver.choosePopupItem(actionTitle(Action::Erase)));
    EXPECT_EQ(erased, std::vector<std::string>{sample::kData});
}

TEST(SidebarTest, ContextMenuOffersUnmountWhileMounted)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    ActionDispatcher& actions = scenario.window->actions();
    actions.setHandler(Action::Mount, [](const Selection&) {});
    actions.setHandler(Action::Unmount, [](const Selection&) {});
    const auto* sidebar = driver.find<views::Sidebar>("sidebar");
    ASSERT_NE(sidebar, nullptr);
    ASSERT_TRUE(driver.click(volume(sample::kStickVolume), MouseButton::Right));
    const auto& entries = sidebar->menuEntries();
    const auto unmount = std::find_if(entries.begin(), entries.end(), [](const views::Sidebar::MenuEntry& entry) {
        return entry.action == Action::Unmount;
    });
    ASSERT_NE(unmount, entries.end());
    EXPECT_TRUE(unmount->enabled);
    const auto lists = [sidebar](Action action) {
        const auto& shown = sidebar->menuEntries();
        return std::any_of(shown.begin(), shown.end(),
                           [action](const views::Sidebar::MenuEntry& entry) { return entry.action == action; });
    };
    EXPECT_FALSE(lists(Action::Lock));
    driver.typeKey(key::Escape);

    // An unlocked encrypted volume can be locked, right after Unmount.
    actions.setHandler(Action::Lock, [](const Selection&) {});
    ASSERT_TRUE(driver.click(volume(sample::kUnlocked), MouseButton::Right));
    const auto& encrypted = sidebar->menuEntries();
    const auto lock = std::find_if(encrypted.begin(), encrypted.end(), [](const views::Sidebar::MenuEntry& entry) {
        return entry.action == Action::Lock;
    });
    ASSERT_NE(lock, encrypted.end());
    EXPECT_TRUE(lock->enabled);
    ASSERT_NE(lock, encrypted.begin());
    EXPECT_EQ((lock - 1)->action, Action::Unmount);
}

TEST(SidebarTest, SelectionOutlivesChanges)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    udisks::Model& model = scenario.model();
    driver.click(volume(sample::kData));

    // Another disk changes: the selection stays.
    model.changeProperties(sample::kStickVolume, udisks::interface::kBlock,
                           {{"IdLabel", udisks::Variant(std::string("Pocket"))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.showsText("Pocket");
    }));
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kData);
    EXPECT_EQ(driver.text("detail:name"), "Data");

    // Its volume goes: the selection goes to its disk.
    driver.click(volume(sample::kStickVolume));
    model.removeInterfaces(sample::kStickVolume,
                           {udisks::interface::kBlock, udisks::interface::kPartition, udisks::interface::kFilesystem});
    EXPECT_TRUE(waitForSelection(scenario, "d:" + sample::kStick));
    EXPECT_TRUE(driver.shows(row("d:" + sample::kStick)));

    // The disk goes too: the first row.
    model.removeInterfaces(sample::kStick, {udisks::interface::kBlock, udisks::interface::kPartitionTable});
    model.removeInterfaces(sample::kStickDrive, {udisks::interface::kDrive});
    EXPECT_TRUE(waitForSelection(scenario, "v:" + sample::kRoot));
}

TEST(SidebarTest, EjectButtonEjectsItsRow)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    ActionDispatcher& actions = scenario.window->actions();
    Selection ejected;
    actions.setHandler(Action::Eject, [&](const Selection& selection) {
        ejected = selection;
        scenario.application.operations().ejectDisk(selection->disk);
    });

    // Only what can go has the button.
    EXPECT_TRUE(driver.shows("sidebar:eject:v:" + sample::kStickVolume));
    EXPECT_TRUE(driver.shows("sidebar:eject:v:" + sample::kImage));
    EXPECT_EQ(driver.countShown("sidebar:eject:v:" + sample::kRoot), 0u);
    EXPECT_EQ(driver.countShown("sidebar:eject:v:" + sample::kData), 0u);
    EXPECT_EQ(driver.countShown("sidebar:eject:d:" + sample::kCard), 0u);

    driver.click(volume(sample::kRoot));
    ASSERT_TRUE(driver.click("sidebar:eject:v:" + sample::kStickVolume));
    ASSERT_TRUE(ejected);
    EXPECT_EQ(ejected->disk, sample::kStick);
    EXPECT_EQ(selectedId(scenario), "v:" + sample::kStickVolume);
    // The volume is unmounted first, on the fake bus.
    udisks::fake::FakeBackend& backend = scenario.backend();
    ASSERT_TRUE(TestDriver::waitFor([&] { return !backend.calls.empty(); }));
    EXPECT_EQ(backend.calls.front().path, sample::kStickVolume);
    EXPECT_EQ(backend.calls.front().method, "Unmount");
}
