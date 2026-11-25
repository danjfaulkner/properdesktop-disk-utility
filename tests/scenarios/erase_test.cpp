// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "dialogs/choice_button.h"
#include "dialogs/volume_format.h"
#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "ui/dialog_window.h"
#include "ui/header_button.h"
#include "ui/keys.h"
#include "ui/switch.h"
#include "ui/text_field.h"

#include <gtest/gtest.h>

#include <tuple>

#include <sys/sysmacros.h>

using namespace properdesktop::ui;
using namespace udisks::fake;
using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

// The window over the sample disks, its backend answering every call: the
// tools for every filesystem but Btrfs and XFS are there, and the calls
// named in failing fail.
class EraseTest : public testing::Test
{
protected:
    void SetUp() override
    {
        scenario.backend().responder = [this](const udisks::Call& call) -> std::optional<udisks::Result> {
            if (call.method == "CanFormat") {
                const std::string type = call.arguments.at(0).value<std::string>();
                const bool missing = type == "btrfs" || type == "xfs";
                return udisks::Result{
                    {}, {}, {Variant(std::tuple<bool, std::string>(!missing, missing ? "mkfs." + type : ""))}};
            }
            if (call.method == "CanResize")
                return udisks::Result{{}, {}, {Variant(std::tuple<bool, uint64_t, std::string>(true, 6, ""))}};
            if (call.method == failing)
                return error(failingError, "The device is busy");
            if (call.method == waiting)
                return std::nullopt;
            return udisks::Result{};
        };
    }

    // The calls made, without the questions about tools.
    std::vector<std::string> calls() const
    {
        std::vector<std::string> result;
        for (const udisks::Call& call : const_cast<EraseTest*>(this)->scenario.backend().calls) {
            if (!call.method.starts_with("Can"))
                result.push_back(call.path + " " + call.method);
        }
        return result;
    }

    const udisks::Call* lastCall(const std::string& method)
    {
        const auto& all = scenario.backend().calls;
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->method == method)
                return &*it;
        }
        return nullptr;
    }

    std::unique_ptr<TestDriver> dialog()
    {
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

    void openErase(const std::string& id)
    {
        scenario.window->select(id);
        scenario.driver.layout();
        scenario.driver.click("toolbar:erase");
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

    Scenario scenario;
    std::string failing;
    std::string failingError = "org.freedesktop.UDisks2.Error.Failed";
    // Left waiting for reply().
    std::string waiting;
};

TEST_F(EraseTest, ErasesAMountedVolume)
{
    openErase("v:" + sample::kStickVolume);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    EXPECT_NE(driver->text("erase:explanation").find("STICK"), std::string::npos);
    EXPECT_EQ(driver->text("erase:name"), "UNTITLED");
    auto* format = driver->find<dialogs::ChoiceButton>("erase:format");
    ASSERT_TRUE(format);
    // The filesystem there now comes first; a volume has no scheme.
    EXPECT_EQ(format->currentValue(), "vfat");
    EXPECT_FALSE(driver->shows("erase:scheme"));
    EXPECT_FALSE(driver->shows("erase:passphrase"));
    // The missing tools' filesystems are listed, not offered.
    for (const auto& item : format->items()) {
        EXPECT_EQ(item.enabled, item.value != "btrfs" && item.value != "xfs") << item.value;
        if (item.value == "xfs")
            EXPECT_NE(item.reason.find("mkfs.xfs"), std::string::npos);
    }

    replaceText(*driver, "erase:name", "Holiday");
    ASSERT_TRUE(enabled(*driver, "erase:erase"));
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_EQ(calls(), (std::vector<std::string>{sample::kStickVolume + " Unmount", sample::kStickVolume + " Format"}));
    const udisks::Call* call = lastCall("Format");
    ASSERT_TRUE(call);
    EXPECT_EQ(call->arguments.at(0).value<std::string>(), "vfat");
    const VariantMap options = call->arguments.at(1).value<VariantMap>();
    EXPECT_EQ(options.at("label").value<std::string>(), "HOLIDAY");
    EXPECT_EQ(options.count("erase"), 0u);
    EXPECT_NE(driver->text("erase:status").find("was erased"), std::string::npos);
    driver->click("erase:done");
    EXPECT_TRUE(TestDriver::waitFor([&] { return !scenario.driver.dialog(); }));

    // The stick pulled out later: the closed dialog raises no alert.
    driver.reset();
    scenario.model().removeInterfaces(sample::kStickVolume, {udisks::interface::kBlock,
                                                             udisks::interface::kPartition,
                                                             udisks::interface::kFilesystem});
    runPosted();
    scenario.driver.layout();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(EraseTest, ChecksTheName)
{
    openErase("v:" + sample::kStickVolume);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    replaceText(*driver, "erase:name", "TWELVE CHARS");
    EXPECT_TRUE(driver->shows("erase:problem"));
    EXPECT_NE(driver->text("erase:problem").find("11 characters"), std::string::npos);
    EXPECT_FALSE(enabled(*driver, "erase:erase"));
    replaceText(*driver, "erase:name", "A:B");
    EXPECT_FALSE(enabled(*driver, "erase:erase"));
    replaceText(*driver, "erase:name", "Fine");
    EXPECT_FALSE(driver->shows("erase:problem"));
    EXPECT_TRUE(enabled(*driver, "erase:erase"));
    // ext4 takes 16 bytes.
    driver->click("erase:format");
    EXPECT_TRUE(driver->choosePopupItem("ext4"));
    replaceText(*driver, "erase:name", "sixteen-letters!");
    EXPECT_TRUE(enabled(*driver, "erase:erase"));
    replaceText(*driver, "erase:name", "seventeen-letters");
    EXPECT_FALSE(enabled(*driver, "erase:erase"));
    EXPECT_TRUE(calls().empty());
}

TEST_F(EraseTest, ErasesAWholeDiskWithItsScheme)
{
    openErase("d:" + sample::kCard);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    ASSERT_TRUE(driver->shows("erase:scheme"));
    EXPECT_EQ(driver->find<dialogs::ChoiceButton>("erase:scheme")->currentValue(), "gpt");
    // A card is carried around: exFAT first.
    EXPECT_EQ(driver->find<dialogs::ChoiceButton>("erase:format")->currentValue(), "exfat");
    driver->click("erase:scheme");
    EXPECT_TRUE(driver->choosePopupItem("Master Boot Record"));
    replaceText(*driver, "erase:name", "CAMERA");
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_EQ(calls(), (std::vector<std::string>{sample::kCard + " Format", sample::kCard + " CreatePartitionAndFormat"}));
    EXPECT_EQ(lastCall("Format")->arguments.at(0).value<std::string>(), "dos");
    const udisks::Call* create = lastCall("CreatePartitionAndFormat");
    EXPECT_EQ(create->arguments.at(0).value<uint64_t>(), dialogs::kMiB);
    EXPECT_EQ(create->arguments.at(3).value<std::string>(), "");
    EXPECT_EQ(create->arguments.at(5).value<std::string>(), "exfat");
    EXPECT_EQ(create->arguments.at(6).value<VariantMap>().at("label").value<std::string>(), "CAMERA");
}

TEST_F(EraseTest, EncryptsWithTheVerifiedPassphrase)
{
    scenario.model().setManager({{"DefaultEncryptionType", Variant(std::string("luks2"))}});
    openErase("v:" + sample::kData);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:format");
    EXPECT_TRUE(driver->choosePopupItem("ext4"));
    driver->click("erase:encrypt");
    ASSERT_TRUE(driver->shows("erase:passphrase"));
    EXPECT_FALSE(enabled(*driver, "erase:erase"));
    replaceText(*driver, "erase:passphrase", "correct horse");
    replaceText(*driver, "erase:verify", "correct hose");
    EXPECT_NE(driver->text("erase:problem").find("don't match"), std::string::npos);
    EXPECT_FALSE(enabled(*driver, "erase:erase"));
    replaceText(*driver, "erase:verify", "correct horse");
    ASSERT_TRUE(enabled(*driver, "erase:erase"));

    // Zeroed first, from the security options.
    driver->click("erase:security");
    EXPECT_TRUE(driver->choosePopupItem("Zero Out Data"));
    EXPECT_TRUE(driver->shows("erase:securityNote"));
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kData + " Format"});
    const VariantMap options = lastCall("Format")->arguments.at(1).value<VariantMap>();
    EXPECT_EQ(options.at("encrypt.passphrase").value<std::string>(), "correct horse");
    EXPECT_EQ(options.at("encrypt.type").value<std::string>(), "luks2");
    EXPECT_EQ(options.at("erase").value<std::string>(), "zero");
}

TEST_F(EraseTest, ShowsTheError)
{
    failing = "Format";
    openErase("v:" + sample::kData);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    // The format had begun: its wipefs may have run.
    EXPECT_NE(driver->text("erase:status").find("only in part"), std::string::npos);
    EXPECT_NE(driver->text("erase:error").find("The device is busy"), std::string::npos);
}

TEST_F(EraseTest, AJobCancelledElsewhereIsReported)
{
    failing = "Format";
    failingError = "org.freedesktop.UDisks2.Error.Cancelled";
    openErase("v:" + sample::kData);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_FALSE(driver->shows("erase:name"));
    EXPECT_NE(driver->text("erase:error").find("was cancelled"), std::string::npos);
}

TEST_F(EraseTest, TheDialogStaysWhileItsVolumeGoes)
{
    waiting = "Format";
    openErase("v:" + sample::kData);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    Window* erase = scenario.driver.dialog();
    driver->click("erase:erase");
    ASSERT_EQ(scenario.backend().pendingCount(), 1u);

    // The disk resets while the format runs: the dialog stays to report it.
    scenario.model().removeInterfaces(sample::kData, {udisks::interface::kBlock, udisks::interface::kPartition,
                                                      udisks::interface::kFilesystem});
    runPosted();
    scenario.driver.layout();
    EXPECT_EQ(scenario.driver.dialog(), erase);
    scenario.backend().reply(0, error("org.freedesktop.UDisks2.Error.Failed", "No such device"));
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_NE(driver->text("erase:error").find("No such device"), std::string::npos);
    driver->click("erase:done");
    driver.reset();
    ASSERT_TRUE(TestDriver::waitFor([&] { return !scenario.driver.dialog(); }));

    // The closed dialog says nothing of what happens to the disk later.
    scenario.model().removeInterfaces(sample::kSata, {udisks::interface::kBlock, udisks::interface::kPartitionTable});
    runPosted();
    scenario.driver.layout();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(EraseTest, AFailureAfterTheNewMapSaysSo)
{
    failing = "CreatePartitionAndFormat";
    openErase("d:" + sample::kCard);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_NE(driver->text("erase:status").find("only in part"), std::string::npos);
    EXPECT_NE(driver->text("erase:error").find("failed"), std::string::npos);
}

TEST_F(EraseTest, TurningDownThePasswordKeepsTheForm)
{
    failing = "Unmount";
    failingError = "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed";
    openErase("v:" + sample::kStickVolume);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    // FAT names show in capitals as they are typed.
    replaceText(*driver, "erase:name", "holiday");
    EXPECT_EQ(driver->text("erase:name"), "HOLIDAY");
    driver->click("erase:encrypt");
    replaceText(*driver, "erase:passphrase", "secret words");
    replaceText(*driver, "erase:verify", "secret words");
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:name");
    }));
    EXPECT_EQ(driver->text("erase:name"), "HOLIDAY");
    EXPECT_EQ(driver->find<dialogs::ChoiceButton>("erase:format")->currentValue(), "vfat");
    EXPECT_TRUE(driver->find<Switch>("erase:encrypt")->isChecked());
    EXPECT_EQ(driver->text("erase:passphrase"), "secret words");
    EXPECT_TRUE(enabled(*driver, "erase:erase"));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kStickVolume + " Unmount"});

    // Cancelled later, it says so rather than failed.
    failing = "Format";
    driver->click("erase:erase");
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_NE(driver->text("erase:error").find("was cancelled"), std::string::npos);
}

TEST_F(EraseTest, StopsAZeroFill)
{
    waiting = "Format";
    openErase("v:" + sample::kData);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:security");
    EXPECT_TRUE(driver->choosePopupItem("Zero Out Data"));
    driver->click("erase:erase");
    driver->layout();
    EXPECT_FALSE(driver->shows("erase:stop"));

    // UDisks2's job on the volume can be cancelled.
    TreeBuilder jobs;
    const std::string job = jobs.addJob("7", "format-erase", {sample::kData}, 0.3, true);
    scenario.model().addInterfaces(job, jobs.objects().at(job));
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:stop");
    }));
    driver->click("erase:stop");
    const udisks::Call* cancel = lastCall("Cancel");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(cancel->path, job);
    EXPECT_EQ(cancel->interface, udisks::interface::kJob);

    // The format then fails as cancelled: part of the volume was zeroed.
    ASSERT_EQ(scenario.backend().pendingCount(), 1u);
    scenario.backend().reply(0, error("org.freedesktop.UDisks2.Error.Cancelled", "cancelled"));
    ASSERT_TRUE(TestDriver::waitFor([&] {
        driver->layout();
        return driver->shows("erase:done");
    }));
    EXPECT_NE(driver->text("erase:status").find("only in part"), std::string::npos);
    EXPECT_NE(driver->text("erase:error").find("was stopped"), std::string::npos);
}

TEST_F(EraseTest, MasterBootRecordPastTwoTiB)
{
    TreeBuilder tree;
    const uint64_t size = 3000000000000;
    const std::string drive = tree.addDrive("Big", {.model = "Big Disk", .size = size, .sortKey = "02"});
    const std::string disk = tree.addDisk("sdx", drive, {.size = size, .deviceNumber = makedev(8, 96)});
    scenario.model().reset(tree.objects());
    ASSERT_TRUE(TestDriver::waitFor([&] {
        scenario.driver.layout();
        return scenario.driver.countShown("sidebar:d:" + disk) == 1;
    }));
    openErase("d:" + disk);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    EXPECT_FALSE(driver->shows("erase:schemeNote"));
    driver->click("erase:scheme");
    EXPECT_TRUE(driver->choosePopupItem("Master Boot Record"));
    ASSERT_TRUE(driver->shows("erase:schemeNote"));
    EXPECT_NE(driver->text("erase:schemeNote").find("stay unused"), std::string::npos);
}

TEST_F(EraseTest, TheSystemIsNotOffered)
{
    scenario.window->select("v:" + sample::kRoot);
    scenario.driver.layout();
    auto* erase = scenario.driver.find<HeaderButton>("toolbar:erase");
    ASSERT_TRUE(erase);
    EXPECT_FALSE(erase->isEnabled());
    EXPECT_FALSE(scenario.window->actions().trigger(Action::Erase));
    EXPECT_FALSE(scenario.window->actions().trigger(Action::Partition));
    EXPECT_FALSE(scenario.window->actions().trigger(Action::RemoveVolume));
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
    EXPECT_TRUE(calls().empty());
}

TEST_F(EraseTest, DeletesAPartitionAfterAsking)
{
    scenario.window->select("v:" + sample::kData);
    scenario.driver.layout();
    scenario.driver.click("toolbar:removeVolume");
    auto driver = dialog();
    ASSERT_TRUE(driver);
    EXPECT_TRUE(driver->showsText("Delete “Data”?"));
    EXPECT_TRUE(calls().empty());
    EXPECT_TRUE(driver->clickText("Delete"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return !calls().empty(); }));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kData + " Delete"});
    const VariantMap options = lastCall("Delete")->arguments.at(0).value<VariantMap>();
    EXPECT_TRUE(options.at("tear-down").value<bool>());

    // The partition goes, as asked: no alert that it is gone.
    driver.reset();
    ASSERT_TRUE(TestDriver::waitFor([&] { return !scenario.driver.dialog(); }));
    scenario.model().removeInterfaces(sample::kData, {udisks::interface::kBlock, udisks::interface::kPartition,
                                                      udisks::interface::kFilesystem});
    runPosted();
    scenario.driver.layout();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}

TEST_F(EraseTest, AnUntouchedSizeIsKept)
{
    scenario.window->select("v:" + sample::kData);
    scenario.driver.layout();
    scenario.driver.click("toolbar:addVolume");
    auto driver = dialog();
    ASSERT_TRUE(driver);
    // Into the size field and out of it: its rounded text is not read back.
    driver->click("addPartition:size");
    driver->click("addPartition:name");
    driver->click("addPartition:add");
    ASSERT_TRUE(TestDriver::waitFor([&] { return !scenario.driver.dialog(); }));
    EXPECT_EQ(lastCall("CreatePartitionAndFormat")->arguments.at(1).value<uint64_t>(),
              largestFreeSpace(scenario.model(), sample::kSata));
}

TEST_F(EraseTest, AddsAPartitionInTheFreeSpace)
{
    scenario.window->select("v:" + sample::kData);
    scenario.driver.layout();
    scenario.driver.click("toolbar:addVolume");
    auto driver = dialog();
    ASSERT_TRUE(driver);
    const uint64_t free = largestFreeSpace(scenario.model(), sample::kSata);
    EXPECT_EQ(driver->text("addPartition:size"), udisks::formatSize(free));
    replaceText(*driver, "addPartition:name", "Backup");
    replaceText(*driver, "addPartition:size", "100 GB");
    driver->typeKey(key::Return);
    driver->click("addPartition:add");
    ASSERT_TRUE(TestDriver::waitFor([&] { return !scenario.driver.dialog(); }));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kSata + " CreatePartitionAndFormat"});
    const udisks::Call* create = lastCall("CreatePartitionAndFormat");
    EXPECT_EQ(create->arguments.at(0).value<uint64_t>(), dialogs::alignUp(dialogs::kMiB + 500000000000));
    EXPECT_EQ(create->arguments.at(1).value<uint64_t>(), dialogs::alignDown(100000000000));
    EXPECT_EQ(create->arguments.at(3).value<std::string>(), "Backup");
    EXPECT_EQ(create->arguments.at(5).value<std::string>(), "ext4");
}

// The newest window over the main one shows the alert that the disk went.
static bool alertsGone(Scenario& scenario)
{
    Window* window = scenario.driver.dialog();
    if (!window)
        return false;
    TestDriver alert(*window, [](const std::string&) {});
    alert.layout();
    return alert.showsText("“STICK” is no longer there.");
}

TEST_F(EraseTest, ADiskSwappedWhileBusyIsReportedAfterwards)
{
    waiting = "Unmount";
    openErase("v:" + sample::kStickVolume);
    auto driver = dialog();
    ASSERT_TRUE(driver);
    driver->click("erase:erase");
    ASSERT_EQ(scenario.backend().pendingCount(), 1u);
    // Another stick in its place while the password is asked for.
    scenario.model().changeProperties(sample::kStickVolume, udisks::interface::kPartition,
                                      {{"UUID", Variant(std::string("another-stick"))}});
    runPosted();
    scenario.backend().reply(0, error("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed", "dismissed"));
    driver.reset();
    // Once the run is over the dialog closes and says why.
    EXPECT_TRUE(TestDriver::waitFor([&] { return alertsGone(scenario); }));
    EXPECT_EQ(calls(), std::vector<std::string>{sample::kStickVolume + " Unmount"});
}

TEST_F(EraseTest, RenameOnASwappedDiskIsReportedAfterwards)
{
    waiting = "SetLabel";
    scenario.window->select("v:" + sample::kStickVolume);
    scenario.driver.layout();
    ASSERT_TRUE(scenario.driver.chooseMenuItem("Edit", "Rename…"));
    auto driver = dialog();
    ASSERT_TRUE(driver);
    replaceText(*driver, "rename:name", "PHOTOS");
    driver->click("rename:rename");
    ASSERT_EQ(scenario.backend().pendingCount(), 1u);
    scenario.model().changeProperties(sample::kStickVolume, udisks::interface::kPartition,
                                      {{"UUID", Variant(std::string("another-stick"))}});
    runPosted();
    scenario.backend().reply(0, error("org.freedesktop.UDisks2.Error.Cancelled", "cancelled"));
    driver.reset();
    EXPECT_TRUE(TestDriver::waitFor([&] { return alertsGone(scenario); }));
}
