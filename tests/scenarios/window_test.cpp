// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "udisks/model.h"
#include "views/context.h"

#include "ui/header_button.h"
#include "ui/menu.h"

#include <gtest/gtest.h>

using namespace properdesktop::ui;
using namespace udisks::fake;

static std::string row(const std::string& id)
{
    return "sidebar:" + id;
}

TEST(WindowTest, ListsTheDisks)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    EXPECT_TRUE(driver.shows("sidebar:section:0"));
    EXPECT_TRUE(driver.shows("sidebar:section:2"));
    EXPECT_TRUE(driver.showsText("Internal"));
    EXPECT_TRUE(driver.showsText("External"));
    EXPECT_TRUE(driver.showsText("Disk Images"));
    EXPECT_TRUE(driver.shows(row("v:" + sample::kRoot)));
    EXPECT_TRUE(driver.shows(row("v:" + sample::kStickVolume)));
    EXPECT_TRUE(driver.shows(row("d:" + sample::kCard)));
    EXPECT_EQ(driver.countShown(row("v:" + sample::kEfi)), 0u);
    // The first volume is selected.
    EXPECT_EQ(driver.text("detail:name"), "ProperDesktop");
    EXPECT_EQ(driver.text("detail:mountPoint"), "/");
}

TEST(WindowTest, SelectionDrivesTheDetailAndTheActions)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    ActionDispatcher& actions = scenario.window->actions();
    Selection erased;
    actions.setHandler(Action::Erase, [&](const Selection& selection) { erased = selection; });

    // The system volume cannot be erased.
    auto* erase = driver.find<HeaderButton>("toolbar:erase");
    ASSERT_NE(erase, nullptr);
    EXPECT_FALSE(erase->isEnabled());

    driver.click(row("v:" + sample::kData));
    EXPECT_EQ(driver.text("detail:name"), "Data");
    EXPECT_TRUE(erase->isEnabled());
    driver.click("toolbar:erase");
    ASSERT_TRUE(erased);
    EXPECT_EQ(erased->block, sample::kData);
    EXPECT_EQ(erased->disk, sample::kSata);
}

TEST(WindowTest, MountAndUnmountShareAButton)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    ActionDispatcher& actions = scenario.window->actions();
    std::vector<std::string> done;
    actions.setHandler(Action::Mount, [&](const Selection& selection) { done.push_back("mount " + selection->block); });
    actions.setHandler(Action::Unmount,
                       [&](const Selection& selection) { done.push_back("unmount " + selection->block); });

    driver.click(row("v:" + sample::kStickVolume));
    EXPECT_NE(driver.find<HeaderButton>("toolbar:mount")->toolTip().find("Unmount"), std::string::npos);
    driver.click("toolbar:mount");
    driver.click(row("v:" + sample::kData));
    driver.click("toolbar:mount");
    EXPECT_EQ(done, (std::vector<std::string>{"unmount " + sample::kStickVolume, "mount " + sample::kData}));
}

TEST(WindowTest, ShowAllDevices)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(row("v:" + sample::kData));
    EXPECT_TRUE(driver.chooseMenuItem("View", "Show All Devices"));
    EXPECT_TRUE(driver.shows(row("d:" + sample::kNvme)));
    EXPECT_TRUE(driver.shows(row("v:" + sample::kEfi)));
    EXPECT_TRUE(driver.shows(row("c:" + sample::kExtended)));
    // The selection stays on the volume.
    EXPECT_EQ(driver.text("detail:name"), "Data");
    EXPECT_EQ(scenario.window->context().viewMode, udisks::ViewMode::AllDevices);

    // A disk shown only here falls back to the first volume in the other view.
    driver.click(row("d:" + sample::kNvme));
    EXPECT_EQ(driver.text("detail:name"), "YMTC PC005 512GB");
    EXPECT_TRUE(driver.chooseMenuItem("View", "Show Only Volumes"));
    EXPECT_EQ(driver.countShown(row("d:" + sample::kNvme)), 0u);
    EXPECT_EQ(driver.text("detail:name"), "ProperDesktop");
}

TEST(WindowTest, FollowsTheDisks)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click(row("v:" + sample::kStickVolume));
    EXPECT_EQ(driver.text("detail:name"), "STICK");

    // Pulled out.
    udisks::Model& model = scenario.model();
    model.removeInterfaces(sample::kStickVolume,
                           {udisks::interface::kBlock, udisks::interface::kPartition, udisks::interface::kFilesystem});
    model.removeInterfaces(sample::kStick, {udisks::interface::kBlock, udisks::interface::kPartitionTable});
    model.removeInterfaces(sample::kStickDrive, {udisks::interface::kDrive});
    EXPECT_TRUE(TestDriver::waitFor([&] { return driver.countShown(row("v:" + sample::kStickVolume)) == 0; }));
    driver.layout();
    EXPECT_EQ(driver.text("detail:name"), "ProperDesktop");

    // A label changed elsewhere shows.
    model.changeProperties(sample::kRoot, udisks::interface::kBlock, {{"IdLabel", udisks::Variant(std::string("Main"))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        driver.layout();
        return driver.text("detail:name") == "Main";
    }));
}

TEST(WindowTest, MainMenu)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    const Menu* menu = driver.window().mainMenu();
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->submenu(0)->role(), MenuRole::Application);
    EXPECT_EQ(menu->submenu(4)->role(), MenuRole::Windows);

    int infos = 0;
    scenario.window->actions().setHandler(Action::Info, [&](const Selection&) { ++infos; });
    EXPECT_TRUE(driver.chooseMenuItem("File", "Get Info"));
    EXPECT_EQ(infos, 1);
}
