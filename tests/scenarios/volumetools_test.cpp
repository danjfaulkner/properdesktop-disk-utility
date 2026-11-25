// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "dialogs/cards.h"
#include "dialogs/unlock.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "udisks/properties.h"
#include "views/context.h"

#include "base/event_loop.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/text_area.h"
#include "ui/text_field.h"

#include <gtest/gtest.h>

#include <tuple>

using namespace properdesktop::ui;
using namespace udisks::fake;
using properdesktop::base::dbus::ObjectPath;
using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

static udisks::Result value(Variant item)
{
    return {{}, {}, {std::move(item)}};
}

static udisks::Result available(bool yes, const std::string& missing = {})
{
    return value(Variant(std::make_tuple(yes, missing)));
}

class VolumeToolsTest : public testing::Test
{
protected:
    FakeBackend& backend() { return scenario.backend(); }

    // The oldest waiting call, answered; what follows has run.
    udisks::Call answer(udisks::Result result = {})
    {
        EXPECT_GT(backend().pendingCount(), 0u);
        if (backend().pendingCount() == 0)
            return {};
        const udisks::Call call = backend().pending(0);
        backend().reply(0, std::move(result));
        runPosted();
        return call;
    }

    std::vector<std::string> methods() const
    {
        std::vector<std::string> names;
        for (const udisks::Call& call : const_cast<VolumeToolsTest*>(this)->backend().calls)
            names.push_back(call.method);
        return names;
    }

    // A driver of the newest window over the given one.
    std::unique_ptr<TestDriver> over(TestDriver& driver)
    {
        Window* window = driver.dialog();
        if (!window) {
            ADD_FAILURE() << "no dialog open";
            return nullptr;
        }
        auto dialog = std::make_unique<TestDriver>(*window,
                                                   [](const std::string& message) { ADD_FAILURE() << message; });
        dialog->setKeyboardFocus(true);
        dialog->layout();
        return dialog;
    }

    void select(const std::string& id)
    {
        scenario.window->select(id);
        scenario.driver.layout();
    }

    std::string infoValue(TestDriver& driver, const std::string& name)
    {
        auto* row = driver.find<dialogs::InfoRow>(name);
        return row ? row->value()->text() : std::string();
    }

    Scenario scenario;
    ActionDispatcher& actions = scenario.window->actions();
};

TEST_F(VolumeToolsTest, UnmountNamesWhyTheVolumeIsBusy)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::Unmount));
    const udisks::Call call = answer(error("org.freedesktop.UDisks2.Error.DeviceBusy",
                                           "Error unmounting /dev/sdb1: target is busy"));
    EXPECT_EQ(call.path, sample::kStickVolume);
    EXPECT_EQ(call.method, "Unmount");
    auto alert = over(scenario.driver);
    ASSERT_TRUE(alert);
    EXPECT_TRUE(alert->showsText("“STICK” couldn’t be unmounted."));
    EXPECT_TRUE(alert->showsText("A program is still using a file on it. Close the files and quit the programs "
                                 "using it, then try again."));
}

TEST_F(VolumeToolsTest, UnmountLocksAnEncryptedVolume)
{
    select("v:" + sample::kUnlocked);
    ASSERT_TRUE(actions.isEnabled(Action::Lock));
    ASSERT_TRUE(actions.trigger(Action::Unmount));
    EXPECT_EQ(answer().path, sample::kCleartext);
    const udisks::Call lock = answer();
    EXPECT_EQ(lock.path, sample::kUnlocked);
    EXPECT_EQ(lock.method, "Lock");
    EXPECT_EQ(methods(), (std::vector<std::string>{"Unmount", "Lock"}));
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(VolumeToolsTest, MountAsksForThePassphrase)
{
    select("v:" + sample::kLocked);
    ASSERT_TRUE(actions.trigger(Action::Mount));
    EXPECT_TRUE(backend().calls.empty());
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_FALSE(dialog->find<DialogButton>("unlock:unlock")->isEnabled());
    dialog->click("unlock:passphrase");
    dialog->typeText("wrong");
    dialog->click("unlock:unlock");

    udisks::Call unlock = answer(
            error("org.freedesktop.UDisks2.Error.Failed",
                  "Error unlocking /dev/sdc2: Failed to activate device: Operation not permitted"));
    EXPECT_EQ(unlock.path, sample::kLocked);
    EXPECT_EQ(unlock.method, "Unlock");
    EXPECT_EQ(unlock.arguments.at(0).value<std::string>(), "wrong");
    dialog->layout();
    EXPECT_EQ(dialog->text("unlock:error"), "The passphrase is incorrect.");
    EXPECT_EQ(dialog->find<TextField>("unlock:passphrase")->text(), "");

    dialog->click("unlock:passphrase");
    dialog->typeText("right");
    dialog->typeKey(properdesktop::ui::key::Return);
    const std::string cleartext = TreeBuilder::blockPath("dm_2d1");
    unlock = answer(value(Variant(ObjectPath{cleartext})));
    EXPECT_EQ(unlock.arguments.at(0).value<std::string>(), "right");
    runPosted();
    const udisks::Call mount = answer();
    EXPECT_EQ(mount.path, cleartext);
    EXPECT_EQ(mount.method, "Mount");
    EXPECT_EQ(methods(), (std::vector<std::string>{"Unlock", "Unlock", "Mount"}));
}

TEST_F(VolumeToolsTest, EjectUnmountsFirst)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::Eject));
    EXPECT_EQ(answer().path, sample::kStickVolume);
    const udisks::Call eject = answer();
    EXPECT_EQ(eject.path, sample::kStickDrive);
    EXPECT_EQ(methods(), (std::vector<std::string>{"Unmount", "Eject"}));

    // Never the running system's disk.
    select("v:" + sample::kRoot);
    EXPECT_FALSE(actions.isEnabled(Action::Eject));
    EXPECT_FALSE(actions.isEnabled(Action::Unmount));
}

TEST_F(VolumeToolsTest, EjectFailureIsShown)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::Eject));
    answer(error("org.freedesktop.UDisks2.Error.DeviceBusy", "target is busy"));
    auto alert = over(scenario.driver);
    ASSERT_TRUE(alert);
    EXPECT_TRUE(alert->showsText("“SanDisk Cruzer Blade” couldn’t be ejected."));
}

TEST_F(VolumeToolsTest, RenameKeepsToTheFilesystemsLimit)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::Rename));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    auto* name = dialog->find<TextField>("rename:name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), "STICK");

    name->setText("twelve chars");
    dialog->layout();
    EXPECT_FALSE(dialog->find<DialogButton>("rename:rename")->isEnabled());
    name->setText("my stick");
    dialog->layout();
    EXPECT_TRUE(dialog->find<DialogButton>("rename:rename")->isEnabled());
    dialog->click("rename:rename");
    const udisks::Call call = answer();
    EXPECT_EQ(call.path, sample::kStickVolume);
    EXPECT_EQ(call.method, "SetLabel");
    EXPECT_EQ(call.arguments.at(0).value<std::string>(), "MY STICK");
    runPosted();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);

    // Ejected after the rename: nothing is said.
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    runPosted();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(VolumeToolsTest, RenameErrorStaysInTheDialog)
{
    select("v:" + sample::kMbrLinux);
    ASSERT_TRUE(actions.trigger(Action::Rename));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    dialog->click("rename:rename");
    answer(error("org.freedesktop.UDisks2.Error.Failed", "e2label failed"));
    dialog->layout();
    EXPECT_EQ(dialog->text("rename:note"), "e2label failed");
    EXPECT_NE(scenario.driver.dialog(), nullptr);
}

TEST_F(VolumeToolsTest, FirstAidUnmountsChecksRepairsAndMountsAgain)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        EXPECT_TRUE(confirm->showsText("Run First Aid on “STICK”?"));
        EXPECT_TRUE(confirm->clickText("Run"));
    }
    runPosted();
    // The disk takes nothing else for the whole run, between calls too.
    EXPECT_FALSE(actions.isEnabled(Action::Erase));
    EXPECT_FALSE(actions.isEnabled(Action::Partition));
    const udisks::Call canCheck = answer(available(true));
    EXPECT_EQ(canCheck.method, "CanCheck");
    EXPECT_EQ(canCheck.arguments.at(0).value<std::string>(), "vfat");
    EXPECT_EQ(answer().method, "Unmount");
    const udisks::Call check = answer(value(Variant(false)));
    EXPECT_EQ(check.method, "Check");
    EXPECT_EQ(check.path, sample::kStickVolume);
    EXPECT_FALSE(actions.isEnabled(Action::Erase));
    EXPECT_EQ(answer(available(true)).method, "CanRepair");
    EXPECT_EQ(answer(value(Variant(true))).method, "Repair");
    const udisks::Call mount = answer();
    EXPECT_EQ(mount.method, "Mount");
    EXPECT_EQ(mount.path, sample::kStickVolume);
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Unmount", "Check", "CanRepair", "Repair", "Mount"}));
    EXPECT_TRUE(actions.isEnabled(Action::Erase));

    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has completed.");
    EXPECT_FALSE(dialog->shows("firstAid:log"));
    dialog->click("firstAid:details");
    dialog->layout();
    EXPECT_TRUE(dialog->shows("firstAid:log"));
    const std::string log = dialog->find<TextArea>("firstAid:log")->text();
    EXPECT_NE(log.find("The file system has errors."), std::string::npos);
    EXPECT_NE(log.find("The file system was repaired."), std::string::npos);
    EXPECT_NE(log.find("Mounting “STICK” again."), std::string::npos);
}

TEST_F(VolumeToolsTest, FirstAidWithoutItsToolIsDisabled)
{
    select("v:" + sample::kData);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(false, "ntfsfix"));
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck"}));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has failed.");
    // The log opens by itself when something went wrong.
    EXPECT_TRUE(dialog->shows("firstAid:log"));

    const ActionState state = actions.state(Action::FirstAid);
    EXPECT_FALSE(state.enabled);
    EXPECT_NE(state.reason.find("ntfsfix"), std::string::npos);
    // Other formats are still checked.
    select("v:" + sample::kStickVolume);
    EXPECT_TRUE(actions.isEnabled(Action::FirstAid));
}

TEST_F(VolumeToolsTest, FirstAidOnADiskChecksEachVolume)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kMbr);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    EXPECT_EQ(answer(available(true)).arguments.at(0).value<std::string>(), "ext4");
    EXPECT_EQ(answer(value(Variant(true))).path, sample::kMbrLinux);
    EXPECT_EQ(answer(available(true)).arguments.at(0).value<std::string>(), "vfat");
    EXPECT_EQ(answer(value(Variant(true))).path, sample::kLogicalFat);
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Check", "CanCheck", "Check"}));
    EXPECT_EQ(backend().pendingCount(), 0u);
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has completed.");
}

TEST_F(VolumeToolsTest, FirstAidStopsWhenThePasswordIsDismissed)
{
    select("v:" + sample::kMbrLinux);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(true));
    answer(error("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed", "dismissed"));
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Check"}));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid was cancelled.");
}

TEST_F(VolumeToolsTest, FirstAidNeverRunsOnTheSystem)
{
    select("v:" + sample::kRoot);
    const ActionState state = actions.state(Action::FirstAid);
    EXPECT_FALSE(state.enabled);
    EXPECT_FALSE(state.reason.empty());
    EXPECT_FALSE(actions.trigger(Action::FirstAid));
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kNvme);
    EXPECT_FALSE(actions.isEnabled(Action::FirstAid));
    EXPECT_TRUE(backend().calls.empty());
}

TEST_F(VolumeToolsTest, InfoListsEveryProperty)
{
    select("v:" + sample::kData);
    ASSERT_TRUE(actions.trigger(Action::Info));
    auto info = over(scenario.driver);
    ASSERT_TRUE(info);
    EXPECT_EQ(infoValue(*info, "info:Block.IdLabel"), "Data");
    EXPECT_EQ(infoValue(*info, "info:Block.Device"), "/dev/sda1");
    EXPECT_TRUE(info->showsText("File System"));
    EXPECT_EQ(info->countShown("info:smart"), 0u);
    EXPECT_TRUE(backend().calls.empty());

    // A label changed elsewhere shows.
    scenario.model().changeProperties(sample::kData, udisks::interface::kBlock,
                                      {{"IdLabel", Variant(std::string("Archive"))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        info->layout();
        return infoValue(*info, "info:Block.IdLabel") == "Archive";
    }));
}

TEST_F(VolumeToolsTest, InfoOpensTheSmartDetails)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kSata);
    scenario.model().changeProperties(sample::kSataDrive, udisks::interface::kAta,
                                      {{"SmartSelftestStatus", Variant(std::string("success"))}});
    ASSERT_TRUE(actions.trigger(Action::Info));
    auto info = over(scenario.driver);
    ASSERT_TRUE(info);
    EXPECT_EQ(infoValue(*info, "info:Drive.Model"), "ST1000LM035");
    info->click("info:smart");
    auto smart = over(*info);
    ASSERT_TRUE(smart);
    EXPECT_EQ(infoValue(*smart, "smart:status"), "Verified");
    EXPECT_EQ(infoValue(*smart, "smart:temperature"), "37 °C");

    using Row = std::tuple<uint8_t, std::string, uint16_t, int32_t, int32_t, int32_t, int64_t, int32_t, VariantMap>;
    const std::vector<Row> rows = {{5, "reallocated-sector-count", 0x33, 5, 5, 10, 12, 3, {}}};
    const udisks::Call read = answer(value(Variant(rows)));
    EXPECT_EQ(read.path, sample::kSataDrive);
    EXPECT_EQ(read.interface, udisks::interface::kAta);
    EXPECT_EQ(read.method, "SmartGetAttributes");
    smart->layout();
    EXPECT_EQ(infoValue(*smart, "smart:attribute:5"), "Failing: 12 sectors");

    smart->click("smart:selftest:start");
    const udisks::Call test = answer();
    EXPECT_EQ(test.method, "SmartSelftestStart");
    EXPECT_EQ(test.arguments.at(0).value<std::string>(), "short");

    scenario.model().changeProperties(sample::kSataDrive, udisks::interface::kAta,
                                      {{"SmartSelftestStatus", Variant(std::string("inprogress"))},
                                       {"SmartSelftestPercentRemaining", Variant(int32_t(70))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        smart->layout();
        return smart->countShown("smart:selftest") == 1
                && infoValue(*smart, "smart:selftest") == "In progress, 70% left";
    }));
    EXPECT_FALSE(smart->find<DialogButton>("smart:selftest:start")->isEnabled());
}

TEST_F(VolumeToolsTest, NvmeHealthIsRead)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kNvme);
    ASSERT_TRUE(actions.trigger(Action::Info));
    auto info = over(scenario.driver);
    ASSERT_TRUE(info);
    info->click("info:smart");
    auto smart = over(*info);
    ASSERT_TRUE(smart);
    const VariantMap health = {{"percent_used", Variant(uint8_t(2))}};
    const udisks::Call read = answer(value(Variant(health)));
    EXPECT_EQ(read.interface, udisks::interface::kNvmeController);
    smart->layout();
    EXPECT_EQ(infoValue(*smart, "smart:health:0"), "2%");
    // The controller reports no self-test state: it runs none.
    EXPECT_EQ(smart->countShown("smart:selftest:start"), 0u);
    scenario.model().changeProperties(sample::kNvmeDrive, udisks::interface::kNvmeController,
                                      {{"SmartSelftestStatus", Variant(std::string("success"))}});
    EXPECT_TRUE(TestDriver::waitFor([&] {
        smart->layout();
        return smart->countShown("smart:selftest:start") == 1;
    }));
    EXPECT_EQ(infoValue(*smart, "smart:powerOn"), "46 days (" + udisks::formatCount(1108) + " hours)");
}

TEST_F(VolumeToolsTest, FirstAidSkipsAVolumeThatWentAway)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kMbr);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(true));
    // Pulled out while the first volume is checked.
    scenario.model().removeInterfaces(sample::kLogicalFat, {udisks::interface::kBlock, udisks::interface::kPartition,
                                                            udisks::interface::kFilesystem});
    EXPECT_EQ(answer(value(Variant(true))).path, sample::kMbrLinux);
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Check"}));
    EXPECT_EQ(backend().pendingCount(), 0u);
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has completed. 1 volume was skipped.");
    EXPECT_NE(dialog->find<TextArea>("firstAid:log")->text().find("is no longer there. Skipped."), std::string::npos);
}

TEST_F(VolumeToolsTest, FirstAidSkipsLockedVolumesWithoutFailing)
{
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kUsb);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    EXPECT_EQ(answer(available(true)).arguments.at(0).value<std::string>(), "ext4");
    EXPECT_EQ(answer().path, sample::kCleartext);
    EXPECT_EQ(answer(value(Variant(true))).method, "Check");
    EXPECT_EQ(answer().method, "Mount");
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Unmount", "Check", "Mount"}));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has completed. 1 volume was skipped.");
}

TEST_F(VolumeToolsTest, FirstAidLeavesOutUnlockedVolumesWithoutAFilesystem)
{
    // The unlocked volume holds an LVM member instead.
    scenario.model().removeInterfaces(sample::kCleartext, {udisks::interface::kFilesystem});
    scenario.model().changeProperties(sample::kCleartext, udisks::interface::kBlock,
                                      {{"IdUsage", Variant(std::string("raid"))},
                                       {"IdType", Variant(std::string("LVM2_member"))}});
    runPosted();
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kUsb);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    // Only the locked volume is left, and it is skipped: nothing is asked.
    EXPECT_TRUE(backend().calls.empty());
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has completed. 1 volume was skipped.");
    EXPECT_EQ(dialog->find<TextArea>("firstAid:log")->text().find("checking tool"), std::string::npos);
}

TEST_F(VolumeToolsTest, UnlockForgetsThePassphrase)
{
    dialogs::UnlockDialog dialog(scenario.window->context(), sample::kLocked, "Vault");
    std::string left = "unset";
    TextField* field = nullptr;
    dialog.onUnlocked = [&](const std::string&) { left = field ? field->text() : "no field"; };
    dialog.show();
    auto driver = over(scenario.driver);
    ASSERT_TRUE(driver);
    field = driver->find<TextField>("unlock:passphrase");
    ASSERT_NE(field, nullptr);
    driver->click("unlock:passphrase");
    driver->typeText("secret");
    driver->click("unlock:unlock");
    answer(value(Variant(ObjectPath{TreeBuilder::blockPath("dm_2d1")})));
    EXPECT_EQ(left, "");
}

TEST_F(VolumeToolsTest, FirstAidLeavesNoGuardBehind)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(true));
    answer();
    answer(value(Variant(true)));
    answer();
    ASSERT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Unmount", "Check", "Mount"}));

    // Ejected afterwards: nothing is said about the finished run.
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    runPosted();
    auto top = over(scenario.driver);
    ASSERT_TRUE(top);
    EXPECT_FALSE(top->showsText("“STICK” is no longer there."));
}

TEST_F(VolumeToolsTest, FirstAidQuestionDeclinedLeavesNoGuard)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Cancel");
    }
    runPosted();
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    runPosted();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(VolumeToolsTest, RenameClosedLeavesNoGuard)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::Rename));
    {
        auto dialog = over(scenario.driver);
        ASSERT_TRUE(dialog);
        dialog->typeKey(properdesktop::ui::key::Escape);
    }
    runPosted();
    ASSERT_EQ(scenario.driver.dialog(), nullptr);
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    runPosted();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(VolumeToolsTest, FirstAidUnansweredToolQuestionIsNotRemembered)
{
    select("v:" + sample::kData);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(error("org.freedesktop.DBus.Error.NoReply", "Did not receive a reply"));
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck"}));
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has failed.");
    const std::string log = dialog->find<TextArea>("firstAid:log")->text();
    EXPECT_NE(log.find("UDisks2 could not tell whether NTFS volumes can be checked."), std::string::npos);
    EXPECT_EQ(log.find("not installed"), std::string::npos);
    // The next try asks again.
    EXPECT_TRUE(actions.isEnabled(Action::FirstAid));
}

TEST_F(VolumeToolsTest, FirstAidFindsTheVolumeAgainBeforeRepairing)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(true));
    answer();
    // Pulled out during the check.
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    answer(value(Variant(false)));
    answer(available(true));
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Unmount", "Check", "CanRepair"}));
    EXPECT_EQ(backend().pendingCount(), 0u);
    auto dialog = over(scenario.driver);
    ASSERT_TRUE(dialog);
    EXPECT_EQ(dialog->text("firstAid:status"), "First Aid process has failed.");
    EXPECT_NE(dialog->find<TextArea>("firstAid:log")->text().find("“STICK” is no longer there."), std::string::npos);
}

TEST_F(VolumeToolsTest, FirstAidFindsTheVolumeAgainBeforeMounting)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    answer(available(true));
    answer();
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    answer(value(Variant(true)));
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck", "Unmount", "Check"}));
    EXPECT_EQ(backend().pendingCount(), 0u);
}

TEST_F(VolumeToolsTest, FirstAidKeepsTheDiskThroughARefusedCall)
{
    select("v:" + sample::kStickVolume);
    ASSERT_TRUE(actions.trigger(Action::FirstAid));
    {
        auto confirm = over(scenario.driver);
        ASSERT_TRUE(confirm);
        confirm->clickText("Run");
    }
    runPosted();
    // Now mounted where the system lives: Unmount is refused without a call.
    scenario.model().changeProperties(sample::kStickVolume, udisks::interface::kFilesystem,
                                      {{"MountPoints", Variant(std::vector<std::vector<uint8_t>>{
                                                               udisks::encodeBytes("/home")})}});
    backend().reply(0, available(true));
    bool busyBetween = false;
    // Runs after the answer and before the refusal's result.
    properdesktop::base::EventLoop::current()->post(
            [&] { busyBetween = scenario.application.operations().isBusy(sample::kStick); });
    runPosted();
    EXPECT_TRUE(busyBetween);
    EXPECT_EQ(methods(), (std::vector<std::string>{"CanCheck"}));
    runPosted();
    EXPECT_FALSE(scenario.application.operations().isBusy(sample::kStick));
}
