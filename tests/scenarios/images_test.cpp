// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "dialogs/copyprogress.h"
#include "dialogs/choice_button.h"
#include "dialogs/diskimages.h"
#include "dialogs/newimage.h"
#include "dialogs/restore.h"
#include "udisks/describe.h"
#include "udisks/tree.h"
#include "views/context.h"
#include "udisks/model.h"

#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/text_field.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

using namespace properdesktop::ui;
using namespace udisks;
using namespace udisks::fake;
using properdesktop::base::dbus::ObjectPath;
using properdesktop::base::dbus::UnixFd;
using FileRequest = dialogs::DiskImages::FileRequest;

static std::vector<std::string> methods(const std::vector<Call>& calls)
{
    std::vector<std::string> names;
    for (const Call& call : calls) {
        if (call.method != "CanFormat")
            names.push_back(call.method + " " + call.path.substr(call.path.rfind('/') + 1));
    }
    return names;
}

static Result value(Variant item)
{
    return {{}, {}, {std::move(item)}};
}

static void fail(const std::string& message)
{
    ADD_FAILURE() << message;
}

class ImagesTest : public testing::Test
{
protected:
    void SetUp() override
    {
        folder = std::filesystem::temp_directory_path() / ("disk-utility-scenario-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder);
        images().setFileDialog([this](const FileRequest& request, dialogs::DiskImages::FileChosen chosen) {
            requests.push_back(request);
            if (!nextPath.empty())
                chosen(nextPath, nextReadOnly);
        });
        scenario.backend().responder = [this](const Call& call) -> std::optional<Result> {
            if (call.method == "CanFormat")
                return value(Variant(std::tuple<bool, std::string>{true, {}}));
            if (call.method == "LoopSetup") {
                if (failLoopSetup)
                    return error("org.freedesktop.UDisks2.Error.Failed", "No free loop device");
                TreeBuilder loops;
                loops.addDisk("loop9", {}, {.size = 64 << 20, .deviceNumber = makedev(7, 9), .idUsage = "filesystem",
                                            .idType = "iso9660", .idLabel = "Disc"});
                loops.setLoop(kLoop, attachedFile);
                loops.setFilesystem(kLoop, {});
                scenario.model().addInterfaces(kLoop, loops.interfaces(kLoop));
                return value(Variant(ObjectPath(kLoop)));
            }
            if (call.method == "CreatePartitionAndFormat")
                return value(Variant(ObjectPath(TreeBuilder::blockPath("loop9p1"))));
            if (call.method == "Mount")
                return value(Variant(std::string("/media/u/Untitled")));
            if (call.method == "OpenForBackup")
                return value(Variant(UnixFd::adopt(::open(file("source").c_str(), O_RDONLY | O_CLOEXEC))));
            if (call.method == "OpenForRestore" && holdRestore)
                return std::nullopt;
            if (call.method == "OpenForRestore" && unwritableTarget)
                return value(Variant(UnixFd::adopt(::open(file("card").c_str(), O_RDONLY | O_CLOEXEC))));
            if (call.method == "OpenForRestore")
                return value(Variant(UnixFd::adopt(::open(file("card").c_str(), O_WRONLY | O_CLOEXEC))));
            return Result{};
        };
    }
    void TearDown() override { std::filesystem::remove_all(folder); }

    dialogs::DiskImages& images() { return scenario.window->diskImages(); }
    std::string file(const std::string& name) const { return (folder / name).string(); }
    std::string write(const std::string& name, const std::string& contents)
    {
        std::ofstream(file(name), std::ios::binary) << contents;
        return file(name);
    }
    static std::string read(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
    // The model's updates and the window's rebuild after them.
    void settle()
    {
        for (int i = 0; i < 4; ++i)
            runPosted();
        scenario.driver.layout();
    }
    std::string selectedDisk() const
    {
        const Selection& selection = scenario.window->actions().selection();
        return selection ? selection->disk : std::string();
    }

    const std::string kLoop = TreeBuilder::blockPath("loop9");
    Scenario scenario;
    std::filesystem::path folder;
    std::vector<FileRequest> requests;
    std::string nextPath;
    bool nextReadOnly = false;
    std::string attachedFile;
    bool failLoopSetup = false;
    // OpenForRestore waits for reply(), keeping the copy running.
    bool holdRestore = false;
    // OpenForRestore hands over a descriptor that cannot be written.
    bool unwritableTarget = false;
};

TEST_F(ImagesTest, OpenDiskImageAttachesAndSelects)
{
    attachedFile = nextPath = write("disc.iso", std::string(4096, 'i'));
    ASSERT_TRUE(scenario.driver.chooseMenuItem("File", "Open Disk Image…"));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].kind, FileRequest::Kind::OpenImage);
    ASSERT_EQ(methods(scenario.backend().calls), std::vector<std::string>{"LoopSetup Manager"});
    // An optical disc image attaches read-only, whatever was chosen.
    EXPECT_TRUE(scenario.backend().calls[0].arguments[1].value<VariantMap>().at("read-only").value<bool>());
    settle();
    EXPECT_EQ(selectedDisk(), kLoop);
    EXPECT_TRUE(scenario.driver.shows("sidebar:v:" + kLoop));
}

TEST_F(ImagesTest, ImagesHandedOverAttachWritable)
{
    attachedFile = write("disk.img", std::string(4096, 'd'));
    scenario.application.open({"file://" + attachedFile});
    ASSERT_EQ(scenario.backend().calls.size(), 1u);
    EXPECT_FALSE(scenario.backend().calls[0].arguments[1].value<VariantMap>().at("read-only").value<bool>());
    settle();
    EXPECT_EQ(selectedDisk(), kLoop);

    // Read-only when asked.
    scenario.backend().calls.clear();
    scenario.model().removeInterfaces(kLoop, {interface::kBlock, interface::kLoop, interface::kFilesystem});
    images().open({attachedFile}, true);
    EXPECT_TRUE(scenario.backend().calls.at(0).arguments[1].value<VariantMap>().at("read-only").value<bool>());
}

TEST_F(ImagesTest, AnAttachedImageIsSelectedAgain)
{
    scenario.application.open({"/home/u/Downloads/debian.iso"});
    EXPECT_TRUE(scenario.backend().calls.empty());
    settle();
    EXPECT_EQ(selectedDisk(), sample::kImage);
}

TEST_F(ImagesTest, AttachFailureIsShown)
{
    failLoopSetup = true;
    images().open({write("broken.img", "x")});
    settle();
    Window* alert = scenario.driver.dialog();
    ASSERT_NE(alert, nullptr);
    TestDriver driver(*alert, fail);
    EXPECT_TRUE(driver.showsText("“broken.img” could not be opened."));
    EXPECT_TRUE(driver.showsText("No free loop device"));
}

TEST_F(ImagesTest, NewBlankImage)
{
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewBlankImage));
    dialogs::NewImageDialog* dialog = images().newImageDialog();
    ASSERT_NE(dialog, nullptr);
    ASSERT_TRUE(dialog->isOpen());
    dialog->setFolder(folder.string());
    attachedFile = file("Backup.img");
    settle();
    Window* window = scenario.driver.dialog();
    ASSERT_NE(window, nullptr);
    TestDriver driver(*window, fail);
    driver.find<TextField>("newImage:saveAs")->setText("Backup");
    driver.find<TextField>("newImage:size")->setText("1");
    driver.layout();
    // 1 MB is too small for anything.
    EXPECT_FALSE(driver.find<View>("newImage:create")->isEnabled());
    EXPECT_NE(driver.text("newImage:note").find("at least"), std::string::npos);

    driver.find<TextField>("newImage:size")->setText("64");
    driver.layout();
    ASSERT_TRUE(driver.find<View>("newImage:create")->isEnabled()) << driver.text("newImage:note");
    driver.click("newImage:create");
    settle();
    EXPECT_EQ(methods(scenario.backend().calls),
              (std::vector<std::string>{"LoopSetup Manager", "Format loop9", "CreatePartitionAndFormat loop9",
                                        "Mount loop9p1"}));
    // Whole MiB.
    EXPECT_EQ(std::filesystem::file_size(file("Backup.img")), 62u << 20);
    EXPECT_FALSE(dialog->isOpen());
    EXPECT_EQ(selectedDisk(), kLoop);
}

TEST_F(ImagesTest, NewBlankImageKeepsAnExistingFile)
{
    write("Untitled.img", "mine");
    scenario.window->actions().trigger(Action::NewBlankImage);
    images().newImageDialog()->setFolder(folder.string());
    settle();
    TestDriver driver(*scenario.driver.dialog(), fail);
    EXPECT_FALSE(driver.find<View>("newImage:create")->isEnabled());
    EXPECT_NE(driver.text("newImage:note").find("already exists"), std::string::npos);
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    EXPECT_EQ(read(file("Untitled.img")), "mine");
}

TEST_F(ImagesTest, ImageFromAMountedVolume)
{
    const std::string data(300'000, 's');
    write("source", data);
    scenario.model().changeProperties(sample::kStickVolume, interface::kBlock, {{"Size", Variant(uint64_t(300'000))}});
    settle();
    scenario.driver.click("sidebar:v:" + sample::kStickVolume);
    nextPath = file("STICK.img");
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewImageFromSelection));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].kind, FileRequest::Kind::SaveImage);
    EXPECT_EQ(requests[0].suggested, "STICK.img");

    // It has to be unmounted first, which is asked.
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    ASSERT_NE(images().confirmation(), nullptr);
    TestDriver alert(*images().confirmation()->window(), fail);
    ASSERT_TRUE(alert.clickText("Unmount"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    EXPECT_EQ(methods(scenario.backend().calls), (std::vector<std::string>{"Unmount sdb1", "OpenForBackup sdb1"}));
    EXPECT_EQ(read(file("STICK.img")), data);
    EXPECT_FALSE(images().copyDialog()->isOpen());
}

TEST_F(ImagesTest, RestoreFromAnImage)
{
    const std::string data(200'000, 'r');
    write("card", std::string(400'000, '-'));
    const std::string image = write("backup.img", data);
    scenario.driver.click("sidebar:d:" + sample::kCard);
    ASSERT_TRUE(scenario.window->actions().isEnabled(Action::Restore));
    nextPath = image;
    scenario.driver.click("toolbar:restore");
    dialogs::RestoreDialog* restore = images().restoreDialog();
    ASSERT_NE(restore, nullptr);
    ASSERT_TRUE(restore->isOpen());
    // Neither the card itself nor the running system is offered.
    for (const dialogs::RestoreDialog::Source& source : restore->sources()) {
        EXPECT_NE(source.endpoint.block, sample::kCard);
        EXPECT_NE(source.endpoint.block, sample::kRoot);
        EXPECT_NE(source.endpoint.block, sample::kNvme);
    }
    EXPECT_FALSE(restore->sources().empty());

    Window* window = scenario.driver.dialog();
    ASSERT_NE(window, nullptr);
    TestDriver driver(*window, fail);
    driver.click("restore:image");
    driver.layout();
    EXPECT_EQ(driver.find<dialogs::ChoiceButton>("restore:source")->currentText(), "backup.img");
    driver.click("restore:restore");
    // The restore asks first, naming what is erased.
    ASSERT_NE(restore->confirmation(), nullptr);
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    TestDriver alert(*restore->confirmation()->window(), fail);
    const std::string name = udisks::displayName(scenario.model(), *scenario.window->actions().selection());
    EXPECT_TRUE(alert.showsText("Erase “" + name + "”?"));
    ASSERT_TRUE(alert.clickText("Erase and Restore"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    EXPECT_EQ(methods(scenario.backend().calls), std::vector<std::string>{"OpenForRestore sdd"});
    EXPECT_EQ(read(file("card")), data + std::string(200'000, '-'));
}

TEST_F(ImagesTest, RestoreRefusesTooLargeSources)
{
    write("huge.img", std::string(1 << 20, 'h'));
    scenario.model().changeProperties(sample::kCard, interface::kBlock, {{"Size", Variant(uint64_t(1000))}});
    settle();
    scenario.driver.click("sidebar:d:" + sample::kCard);
    nextPath = file("huge.img");
    scenario.window->actions().trigger(Action::Restore);
    TestDriver driver(*scenario.driver.dialog(), fail);
    driver.click("restore:image");
    driver.layout();
    EXPECT_FALSE(driver.find<View>("restore:restore")->isEnabled());
    EXPECT_NE(driver.text("restore:problem").find("larger"), std::string::npos);
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
}

TEST_F(ImagesTest, NothingRestoresOntoTheSystem)
{
    scenario.driver.click("sidebar:v:" + sample::kRoot);
    EXPECT_FALSE(scenario.window->actions().isEnabled(Action::Restore));
    EXPECT_FALSE(scenario.window->actions().trigger(Action::Restore));
    EXPECT_EQ(images().restoreDialog(), nullptr);
}

TEST_F(ImagesTest, OneCopyAtATime)
{
    write("card", std::string(1000, '-'));
    nextPath = write("backup.img", std::string(500, 'r'));
    holdRestore = true;
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    TestDriver driver(*scenario.driver.dialog(), fail);
    driver.click("restore:image");
    driver.click("restore:restore");
    ASSERT_NE(restore->confirmation(), nullptr);
    TestDriver alert(*restore->confirmation()->window(), fail);
    ASSERT_TRUE(alert.clickText("Erase and Restore"));
    settle();
    ASSERT_TRUE(images().isCopying());
    ASSERT_EQ(scenario.backend().pendingCount(), 1u);

    // Another copy would end this one: both copies wait.
    ActionDispatcher& actions = scenario.window->actions();
    EXPECT_FALSE(actions.isEnabled(Action::Restore));
    // On another disk too.
    scenario.driver.click("sidebar:v:" + sample::kStickVolume);
    EXPECT_FALSE(actions.isEnabled(Action::Restore));
    EXPECT_FALSE(actions.isEnabled(Action::NewImageFromSelection));
    EXPECT_FALSE(actions.state(Action::NewImageFromSelection).reason.empty());
    EXPECT_FALSE(actions.trigger(Action::NewImageFromSelection));
    EXPECT_EQ(requests.size(), 1u);

    scenario.backend().reply(0, value(Variant(UnixFd::adopt(::open(file("card").c_str(), O_WRONLY | O_CLOEXEC)))));
    ASSERT_TRUE(TestDriver::waitFor([&] { return !images().isCopying(); }));
    settle();
    EXPECT_TRUE(actions.isEnabled(Action::NewImageFromSelection));
    EXPECT_EQ(read(file("card")), std::string(500, 'r') + std::string(500, '-'));
}

TEST_F(ImagesTest, TheConfirmationKeepsItsTarget)
{
    write("card", std::string(1000, '-'));
    nextPath = write("backup.img", std::string(500, 'r'));
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    {
        TestDriver driver(*scenario.driver.dialog(), fail);
        driver.click("restore:image");
        driver.click("restore:restore");
    }
    ASSERT_NE(restore->confirmation(), nullptr);

    // Another target replaces the question about the card.
    // Copies: the tree is built again as the disks change.
    const udisks::Node* found = udisks::findNode(scenario.window->context().tree, "v:" + sample::kMbrLinux);
    ASSERT_NE(found, nullptr);
    const udisks::Node other = *found;
    found = udisks::findNode(scenario.window->context().tree, "d:" + sample::kCard);
    ASSERT_NE(found, nullptr);
    const udisks::Node card = *found;
    restore->show(other);
    EXPECT_EQ(restore->confirmation(), nullptr);

    // A card that changed while the question was open, another one at its
    // path, takes the question and the dialog away, and is not written.
    restore->show(card);
    restore->setImage(nextPath);
    restore->restore();
    ASSERT_NE(restore->confirmation(), nullptr);
    scenario.model().changeProperties(sample::kCard, interface::kBlock, {{"Size", Variant(uint64_t(100))}});
    EXPECT_EQ(restore->confirmation(), nullptr);
    EXPECT_FALSE(restore->isOpen());
    settle();
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    EXPECT_FALSE(images().isCopying());
    Window* error = scenario.driver.dialog();
    ASSERT_NE(error, nullptr);
    TestDriver errorDriver(*error, fail);
    EXPECT_TRUE(errorDriver.showsText("“SD Card” is no longer there."));
    EXPECT_EQ(read(file("card")), std::string(1000, '-'));
}

TEST_F(ImagesTest, AMountedSourceStaysUnmounted)
{
    scenario.model().changeProperties(sample::kCard, interface::kBlock, {{"Size", Variant(uint64_t(1) << 42)}});
    settle();
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    TestDriver driver(*scenario.driver.dialog(), fail);
    const auto& sources = restore->sources();
    const auto stick = std::find_if(sources.begin(), sources.end(), [](const dialogs::RestoreDialog::Source& source) {
        return source.endpoint.block == sample::kStickVolume;
    });
    ASSERT_NE(stick, sources.end());
    driver.find<dialogs::ChoiceButton>("restore:source")->setCurrentIndex(int(stick - sources.begin()));
    restore->restore();
    ASSERT_NE(restore->confirmation(), nullptr);
    TestDriver alert(*restore->confirmation()->window(), fail);
    const std::string card = udisks::displayName(scenario.model(), *scenario.window->actions().selection());
    EXPECT_TRUE(alert.showsText("Everything on “" + card + "” is replaced with the contents of “STICK”. This cannot "
                                "be undone. “STICK” is unmounted before it is read, and stays so afterwards."));
}

TEST_F(ImagesTest, NoImageOntoTheVolumeItself)
{
    scenario.driver.click("sidebar:v:" + sample::kStickVolume);
    nextPath = "/media/u/STICK/Backups/STICK.img";
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewImageFromSelection));
    settle();
    EXPECT_EQ(images().confirmation(), nullptr);
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    Window* error = scenario.driver.dialog();
    ASSERT_NE(error, nullptr);
    TestDriver errorDriver(*error, fail);
    EXPECT_TRUE(errorDriver.showsText("The image could not be created."));
}

TEST_F(ImagesTest, NoAlertForATargetEjectedAfterwards)
{
    write("card", std::string(1000, '-'));
    nextPath = write("backup.img", std::string(500, 'r'));
    scenario.driver.click("sidebar:d:" + sample::kCard);

    // Closed with Escape.
    scenario.window->actions().trigger(Action::Restore);
    {
        TestDriver driver(*scenario.driver.dialog(), fail);
        driver.typeKey(key::Escape);
    }
    settle();
    EXPECT_FALSE(images().restoreDialog()->isOpen());

    // Restored, and done.
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    {
        TestDriver driver(*scenario.driver.dialog(), fail);
        driver.click("restore:image");
        driver.click("restore:restore");
    }
    ASSERT_NE(restore->confirmation(), nullptr);
    TestDriver alert(*restore->confirmation()->window(), fail);
    ASSERT_TRUE(alert.clickText("Erase and Restore"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    EXPECT_FALSE(images().copyDialog()->isOpen());
    ASSERT_EQ(scenario.driver.dialog(), nullptr);

    // The card is ejected later: nothing is open on it to complain.
    scenario.model().removeInterfaces(sample::kCard, {interface::kBlock});
    settle();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
    EXPECT_FALSE(scenario.driver.showsText("is no longer there"));
}

TEST_F(ImagesTest, AFailedRestoreSaysTheTargetIsDamaged)
{
    write("card", std::string(1000, '-'));
    nextPath = write("backup.img", std::string(500, 'r'));
    unwritableTarget = true;
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    {
        TestDriver driver(*scenario.driver.dialog(), fail);
        driver.click("restore:image");
        driver.click("restore:restore");
    }
    TestDriver alert(*restore->confirmation()->window(), fail);
    ASSERT_TRUE(alert.clickText("Erase and Restore"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    ASSERT_TRUE(images().copyDialog()->isOpen());
    TestDriver copy(*scenario.driver.dialog(), fail);
    EXPECT_NE(copy.text("copy:error").find("overwritten in part"), std::string::npos) << copy.text("copy:error");
}

TEST_F(ImagesTest, AFailedImageSaysTheFileWasRemoved)
{
    // The volume says it is larger than what can be read from it.
    write("source", std::string(1000, 's'));
    scenario.model().changeProperties(sample::kData, interface::kBlock, {{"Size", Variant(uint64_t(300'000))}});
    settle();
    scenario.driver.click("sidebar:v:" + sample::kData);
    nextPath = file("Data.img");
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewImageFromSelection));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    ASSERT_TRUE(images().copyDialog()->isOpen());
    TestDriver copy(*scenario.driver.dialog(), fail);
    EXPECT_NE(copy.text("copy:error").find("incomplete image file was removed"), std::string::npos)
        << copy.text("copy:error");
    EXPECT_FALSE(std::filesystem::exists(file("Data.img")));
}

TEST_F(ImagesTest, ARemovedSourceLeavesNothingChosen)
{
    scenario.model().changeProperties(sample::kCard, interface::kBlock, {{"Size", Variant(uint64_t(1) << 42)}});
    settle();
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    TestDriver driver(*scenario.driver.dialog(), fail);
    const auto& sources = restore->sources();
    const auto stick = std::find_if(sources.begin(), sources.end(), [](const dialogs::RestoreDialog::Source& source) {
        return source.endpoint.block == sample::kStickVolume;
    });
    ASSERT_NE(stick, sources.end());
    auto* choice = driver.find<dialogs::ChoiceButton>("restore:source");
    choice->setCurrentIndex(int(stick - sources.begin()));
    ASSERT_TRUE(choice->onActivated);
    choice->onActivated(choice->currentIndex());
    driver.layout();
    ASSERT_TRUE(driver.find<View>("restore:restore")->isEnabled()) << driver.text("restore:problem");

    // The stick is pulled out.
    scenario.model().removeInterfaces(sample::kStickVolume,
                                      {interface::kBlock, interface::kPartition, interface::kFilesystem});
    settle();
    driver.layout();
    EXPECT_EQ(choice->currentIndex(), -1);
    EXPECT_FALSE(driver.find<View>("restore:restore")->isEnabled());
    EXPECT_EQ(driver.text("restore:problem"), "“STICK” is no longer there. Choose another source.");

    // An unplug comes in several batches, and other disks change meanwhile:
    // nothing gets chosen in its place.
    scenario.model().removeInterfaces(sample::kStick, {interface::kPartitionTable});
    settle();
    scenario.model().removeInterfaces(sample::kStick, {interface::kBlock});
    settle();
    scenario.model().changeProperties(sample::kMbrLinux, interface::kBlock,
                                      {{"IdLabel", Variant(std::string("Renamed"))}});
    settle();
    driver.layout();
    EXPECT_EQ(choice->currentIndex(), -1);
    EXPECT_FALSE(driver.find<View>("restore:restore")->isEnabled());
    EXPECT_EQ(driver.text("restore:problem"), "“STICK” is no longer there. Choose another source.");

    // Until a source is picked again.
    choice->setCurrentIndex(0);
    choice->onActivated(0);
    driver.layout();
    EXPECT_NE(driver.text("restore:problem"), "“STICK” is no longer there. Choose another source.");
}

TEST_F(ImagesTest, AnotherStickAtTheSamePathIsNotTheSource)
{
    scenario.model().changeProperties(sample::kCard, interface::kBlock, {{"Size", Variant(uint64_t(1) << 42)}});
    settle();
    scenario.driver.click("sidebar:d:" + sample::kCard);
    scenario.window->actions().trigger(Action::Restore);
    dialogs::RestoreDialog* restore = images().restoreDialog();
    TestDriver driver(*scenario.driver.dialog(), fail);
    const auto& sources = restore->sources();
    const auto stick = std::find_if(sources.begin(), sources.end(), [](const dialogs::RestoreDialog::Source& source) {
        return source.endpoint.block == sample::kStickVolume;
    });
    ASSERT_NE(stick, sources.end());
    auto* choice = driver.find<dialogs::ChoiceButton>("restore:source");
    choice->setCurrentIndex(int(stick - sources.begin()));
    choice->onActivated(choice->currentIndex());
    driver.layout();
    ASSERT_TRUE(driver.find<View>("restore:restore")->isEnabled()) << driver.text("restore:problem");

    // In one change the stick is another one: same paths, another drive.
    scenario.model().changeProperties(sample::kStickDrive, interface::kDrive,
                                      {{"Serial", Variant(std::string("OTHER"))},
                                       {"Id", Variant(std::string("Other-Stick-OTHER"))},
                                       {"TimeDetected", Variant(uint64_t(1'900'000'000'000'000))}});
    settle();
    driver.layout();
    EXPECT_EQ(choice->currentIndex(), -1);
    EXPECT_FALSE(driver.find<View>("restore:restore")->isEnabled());
    EXPECT_NE(driver.text("restore:problem").find("no longer there"), std::string::npos);
    restore->restore();
    EXPECT_EQ(restore->confirmation(), nullptr);
}

TEST_F(ImagesTest, TheUnmountQuestionGoesWithItsVolume)
{
    scenario.driver.click("sidebar:v:" + sample::kStickVolume);
    nextPath = file("STICK.img");
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewImageFromSelection));
    ASSERT_NE(images().confirmation(), nullptr);

    // Pulled out while asked: the question goes and the window says why.
    scenario.model().removeInterfaces(sample::kStickVolume,
                                      {interface::kBlock, interface::kPartition, interface::kFilesystem});
    settle();
    EXPECT_EQ(images().confirmation(), nullptr);
    EXPECT_TRUE(methods(scenario.backend().calls).empty());
    Window* alert = scenario.driver.dialog();
    ASSERT_NE(alert, nullptr);
    TestDriver driver(*alert, fail);
    EXPECT_TRUE(driver.showsText("“STICK” is no longer there."));
}

TEST_F(ImagesTest, NoAlertForAnImagedVolumeEjectedAfterwards)
{
    const std::string data(300'000, 's');
    write("source", data);
    scenario.model().changeProperties(sample::kStickVolume, interface::kBlock, {{"Size", Variant(uint64_t(300'000))}});
    settle();
    scenario.driver.click("sidebar:v:" + sample::kStickVolume);
    nextPath = file("STICK.img");
    ASSERT_TRUE(scenario.window->actions().trigger(Action::NewImageFromSelection));
    TestDriver alert(*images().confirmation()->window(), fail);
    ASSERT_TRUE(alert.clickText("Unmount"));
    ASSERT_TRUE(TestDriver::waitFor([&] { return images().copyDialog() && !images().copyDialog()->isRunning(); }));
    settle();
    ASSERT_EQ(scenario.driver.dialog(), nullptr);

    scenario.model().removeInterfaces(sample::kStickVolume,
                                      {interface::kBlock, interface::kPartition, interface::kFilesystem});
    settle();
    EXPECT_EQ(scenario.driver.dialog(), nullptr);
}
