// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "fake_udisks.h"

#include "actions.h"
#include "images/blankimage.h"
#include "images/imagefile.h"
#include "images/transfer.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "udisks/tree.h"

#include "base/event_loop.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>

using namespace images;
using namespace udisks;
using namespace udisks::fake;
using properdesktop::base::dbus::UnixFd;

static constexpr uint64_t kMiB = 1 << 20;
static constexpr uint64_t kVolumeSize = 256 * 1024;

static std::vector<std::string> methods(const std::vector<Call>& calls)
{
    std::vector<std::string> names;
    for (const Call& call : calls)
        names.push_back(call.method + " " + call.path.substr(call.path.rfind('/') + 1));
    return names;
}

static Result value(Variant item)
{
    return {{}, {}, {std::move(item)}};
}

class TransferTest : public testing::Test
{
protected:
    void SetUp() override
    {
        folder = std::filesystem::temp_directory_path() / ("disk-utility-transfer-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder);

        // sdx: a USB stick with a mounted 256 KiB volume. sdy: a larger stick
        // holding an unlocked, mounted encrypted volume.
        const std::string stick = tree.addDrive("Stick", {.size = 2 * kMiB, .connectionBus = "usb", .removable = true});
        sdx = tree.addDisk("sdx", stick, {.size = 2 * kMiB, .deviceNumber = makedev(8, 96)}, "gpt");
        sdx1 = tree.addPartition(sdx, "sdx1", 1, kMiB,
                                 {.size = kVolumeSize, .deviceNumber = makedev(8, 97), .idUsage = "filesystem",
                                  .idType = "ext4", .idLabel = "Small"});
        tree.setFilesystem(sdx1, {"/media/u/Small"});
        const std::string big = tree.addDrive("Big", {.size = 4 * kMiB, .connectionBus = "usb", .removable = true});
        sdy = tree.addDisk("sdy", big, {.size = 4 * kMiB, .deviceNumber = makedev(8, 112)}, "gpt");
        sdy1 = tree.addPartition(sdy, "sdy1", 1, kMiB,
                                 {.size = 2 * kMiB, .deviceNumber = makedev(8, 113), .idUsage = "crypto",
                                  .idType = "crypto_LUKS"});
        cleartext = tree.addCleartext("dm_2d5", sdy1,
                                      {.size = 2 * kMiB - 16 * 1024, .deviceNumber = makedev(254, 5),
                                       .idUsage = "filesystem", .idType = "ext4"});
        tree.setFilesystem(cleartext, {"/media/u/Locked"});
        // The system is elsewhere.
        model.setSystemDevices({{}, true});
        model.reset(tree.objects());

        source = write("volume", data(kVolumeSize));
        restoreTarget = write("disk", std::string(4 * kMiB, 'x'));
        backend.responder = [this](const Call& call) -> std::optional<Result> {
            if (call.method == "OpenForBackup")
                return value(Variant(UnixFd::adopt(::open(source.c_str(), O_RDONLY | O_CLOEXEC))));
            if (call.method == "OpenForRestore")
                return value(Variant(UnixFd::adopt(::open(restoreTarget.c_str(), O_WRONLY | O_CLOEXEC))));
            if (call.method == "Unmount" || call.method == "Lock")
                return Result{};
            return std::nullopt;
        };
    }
    void TearDown() override { std::filesystem::remove_all(folder); }

    std::string file(const std::string& name) { return (folder / name).string(); }
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
    static std::string data(size_t size)
    {
        std::string bytes(size, '\0');
        for (size_t i = 0; i < size; ++i)
            bytes[i] = char((i * 7 + i / 251) & 0xff);
        return bytes;
    }

    // Starts the transfer, then runs the loop, and the copy's thread, until
    // it is over.
    static std::optional<Transfer::Outcome> run(Transfer& transfer, const Endpoint& from, const Endpoint& to)
    {
        std::optional<Transfer::Outcome> outcome;
        transfer.onFinished = [&](const Transfer::Outcome& o) { outcome = o; };
        transfer.start(from, to);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!outcome && std::chrono::steady_clock::now() < end) {
            runPosted();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return outcome;
    }

    properdesktop::base::EventLoop loop;
    std::filesystem::path folder;
    TreeBuilder tree;
    Model model;
    FakeBackend backend;
    Operations operations{model, backend};
    std::string sdx, sdx1, sdy, sdy1, cleartext;
    std::string source;
    std::string restoreTarget;
};

TEST_F(TransferTest, WhatIsOnABlock)
{
    EXPECT_EQ(mountedOn(model, sdx), std::vector<std::string>{sdx1});
    EXPECT_EQ(mountedOn(model, sdx1), std::vector<std::string>{sdx1});
    EXPECT_EQ(mountedOn(model, sdy), std::vector<std::string>{cleartext});
    EXPECT_EQ(unlockedOn(model, sdy), std::vector<std::string>{sdy1});
    EXPECT_TRUE(unlockedOn(model, sdx).empty());
    EXPECT_EQ(sizeOf(model, Endpoint::ofBlock(sdx1)), kVolumeSize);
    EXPECT_EQ(sizeOf(model, Endpoint::ofFile(source)), kVolumeSize);
    EXPECT_FALSE(sizeOf(model, Endpoint::ofFile(file("missing"))));
    EXPECT_FALSE(sizeOf(model, Endpoint::ofBlock("/nothing")));

    EXPECT_TRUE(overlaps(model, sdx, sdx1));
    EXPECT_TRUE(overlaps(model, sdx1, sdx));
    EXPECT_TRUE(overlaps(model, cleartext, sdy));
    EXPECT_FALSE(overlaps(model, sdx1, sdy1));
}

TEST_F(TransferTest, RestoreProblems)
{
    EXPECT_EQ(restoreProblem(model, Endpoint::ofBlock(sdx1), sdy), "");
    EXPECT_EQ(restoreProblem(model, Endpoint::ofFile(source), sdy1), "");
    EXPECT_NE(restoreProblem(model, Endpoint::ofBlock(sdx1), sdx1), "");
    EXPECT_NE(restoreProblem(model, Endpoint::ofBlock(sdx1), sdx), "");
    EXPECT_NE(restoreProblem(model, Endpoint::ofBlock(sdy), sdy1), "");
    // Larger than the volume.
    EXPECT_NE(restoreProblem(model, Endpoint::ofBlock(sdy), sdx1).find("larger"), std::string::npos);
    EXPECT_NE(restoreProblem(model, Endpoint::ofFile(file("missing")), sdy), "");
    EXPECT_NE(restoreProblem(model, Endpoint::ofBlock(sdx1), "/gone"), "");
    write("empty", "");
    EXPECT_NE(restoreProblem(model, Endpoint::ofFile(file("empty")), sdy), "");
}

TEST_F(TransferTest, ImagesAMountedVolume)
{
    Transfer transfer(model, operations);
    std::vector<Transfer::Step> steps;
    transfer.onStep = [&](Transfer::Step step) { steps.push_back(step); };
    const std::optional<Transfer::Outcome> outcome = run(transfer, Endpoint::ofBlock(sdx1),
                                                         Endpoint::ofFile(file("Small.img")));
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok) << outcome->error;
    EXPECT_EQ(outcome->copied, kVolumeSize);
    EXPECT_EQ(methods(backend.calls), (std::vector<std::string>{"Unmount sdx1", "OpenForBackup sdx1"}));
    EXPECT_EQ(steps, (std::vector<Transfer::Step>{Transfer::Step::Unmounting, Transfer::Step::Opening,
                                                  Transfer::Step::Copying}));
    EXPECT_EQ(read(file("Small.img")), read(source));
}

TEST_F(TransferTest, BothBlocksAreBusyWhileCopying)
{
    Transfer transfer(model, operations);
    std::vector<bool> busyWhileCopying;
    transfer.onStep = [&](Transfer::Step step) {
        if (step == Transfer::Step::Copying)
            busyWhileCopying = {operations.isBusy(sdx1), operations.isBusy(sdx), operations.isBusy(sdy),
                                operations.isBusy(sdy1)};
    };
    int busyChanges = 0;
    auto subscription = operations.busyChanged.connect([&] { ++busyChanges; });
    const std::optional<Transfer::Outcome> outcome = run(transfer, Endpoint::ofBlock(sdx1), Endpoint::ofBlock(sdy));
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok) << outcome->error;
    // The volume, its disk, the target disk and what is on it.
    EXPECT_EQ(busyWhileCopying, (std::vector<bool>{true, true, true, true}));
    EXPECT_GT(busyChanges, 0);
    EXPECT_FALSE(operations.isBusy(sdx1));
    EXPECT_FALSE(operations.isBusy(sdy));

    // Nothing else goes out on them meanwhile, and the toolbar greys out.
    Operations::Hold hold = operations.hold(sdy);
    const std::vector<SectionNodes> nodes = buildTree(model, ViewMode::AllDevices);
    ASSERT_NE(findNode(nodes, "d:" + sdy), nullptr);
    EXPECT_FALSE(actionState(Action::Erase, model, &operations, *findNode(nodes, "d:" + sdy)).enabled);
    EXPECT_TRUE(actionState(Action::Erase, model, &operations, *findNode(nodes, "d:" + sdx)).enabled);
    std::optional<Result> refused;
    operations.mount(cleartext, [&](const Result& result) { refused = result; });
    runPosted();
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->errorName, Operations::kBusyError);
    hold.reset();
    EXPECT_FALSE(operations.isBusy(sdy));
    EXPECT_TRUE(actionState(Action::Erase, model, &operations, *findNode(nodes, "d:" + sdy)).enabled);
}

TEST_F(TransferTest, AnotherDiskAtTheTargetsPath)
{
    // Chosen, then the stick is swapped for another before the copy starts.
    const Endpoint target = Endpoint::ofBlock(model, sdy);
    model.changeProperties(model.object(sdy)->block->drive, interface::kDrive,
                           {{"Serial", Variant(std::string("ANOTHER"))}});
    Transfer transfer(model, operations);
    const std::optional<Transfer::Outcome> outcome = run(transfer, Endpoint::ofFile(source), target);
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_EQ(outcome->error, "The disk was removed or replaced by another.");
    EXPECT_TRUE(backend.calls.empty());
}

TEST_F(TransferTest, RestoresAnImageOntoADisk)
{
    Transfer transfer(model, operations);
    const std::optional<Transfer::Outcome> outcome = run(transfer, Endpoint::ofFile(source), Endpoint::ofBlock(sdy));
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok) << outcome->error;
    // Unmounted, then locked, then opened.
    EXPECT_EQ(methods(backend.calls),
              (std::vector<std::string>{"Unmount dm_2d5", "Lock sdy1", "OpenForRestore sdy"}));
    const std::string written = read(restoreTarget);
    EXPECT_EQ(written.substr(0, kVolumeSize), read(source));
    // The rest of the disk is left as it was.
    EXPECT_EQ(written.substr(kVolumeSize), std::string(4 * kMiB - kVolumeSize, 'x'));
}

TEST_F(TransferTest, RefusalsSendNothing)
{
    for (const std::string& target : {sdx1, sdx}) {
        Transfer transfer(model, operations);
        const std::optional<Transfer::Outcome> outcome = run(transfer, Endpoint::ofBlock(sdx1),
                                                             Endpoint::ofBlock(target));
        ASSERT_TRUE(outcome);
        EXPECT_FALSE(outcome->ok);
        EXPECT_NE(outcome->error, "");
    }
    Transfer tooLarge(model, operations);
    ASSERT_TRUE(run(tooLarge, Endpoint::ofBlock(sdy), Endpoint::ofBlock(sdx1)));
    EXPECT_TRUE(backend.calls.empty());

    // The running system's volume cannot be unmounted, so nothing is sent.
    Model laptop;
    loadSample(laptop);
    Operations laptopOperations(laptop, backend);
    Transfer system(laptop, laptopOperations);
    write("big", std::string(1024, 'a'));
    const std::optional<Transfer::Outcome> outcome = run(system, Endpoint::ofFile(file("big")),
                                                         Endpoint::ofBlock(sample::kRoot));
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_TRUE(backend.calls.empty());
}

TEST_F(TransferTest, FailuresRemoveTheImageFile)
{
    // The source is shorter than the volume says.
    write("volume", data(1000));
    Transfer shortSource(model, operations);
    std::optional<Transfer::Outcome> outcome = run(shortSource, Endpoint::ofBlock(sdx1),
                                                   Endpoint::ofFile(file("Short.img")));
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_NE(outcome->error, "");
    EXPECT_FALSE(std::filesystem::exists(file("Short.img")));

    backend.calls.clear();
    backend.responder = [](const Call& call) -> std::optional<Result> {
        if (call.method == "OpenForBackup")
            return error("org.freedesktop.UDisks2.Error.Failed", "Device busy");
        return Result{};
    };
    Transfer busy(model, operations);
    outcome = run(busy, Endpoint::ofBlock(sdx1), Endpoint::ofFile(file("Busy.img")));
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->error, "Device busy");
    EXPECT_FALSE(std::filesystem::exists(file("Busy.img")));
}

TEST_F(TransferTest, CancelledBeforeTheCopy)
{
    backend.responder = {};
    Transfer transfer(model, operations);
    std::optional<Transfer::Outcome> outcome;
    transfer.onFinished = [&](const Transfer::Outcome& o) { outcome = o; };
    transfer.start(Endpoint::ofBlock(sdx1), Endpoint::ofFile(file("Small.img")));
    ASSERT_EQ(backend.pendingCount(), 1u);
    transfer.cancel();
    backend.replyAll();
    runPosted();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->cancelled);
    EXPECT_EQ(methods(backend.calls), std::vector<std::string>{"Unmount sdx1"});
    EXPECT_FALSE(std::filesystem::exists(file("Small.img")));

    // The password dialog dismissed.
    backend.calls.clear();
    backend.responder = [](const Call&) -> std::optional<Result> {
        return error("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed", "Dismissed");
    };
    Transfer dismissed(model, operations);
    const std::optional<Transfer::Outcome> second = run(dismissed, Endpoint::ofFile(source), Endpoint::ofBlock(sdy));
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->cancelled);
    EXPECT_EQ(backend.calls.size(), 1u);
}

class BlankImageTest : public TransferTest
{
protected:
    void SetUp() override
    {
        TransferTest::SetUp();
        loopPath = TreeBuilder::blockPath("loop7");
        partitionPath = TreeBuilder::blockPath("loop7p1");
        backend.responder = [this](const Call& call) -> std::optional<Result> {
            if (call.method == "LoopSetup") {
                TreeBuilder loops;
                loops.addDisk("loop7", {}, {.size = 64 * kMiB, .deviceNumber = makedev(7, 7)});
                loops.setLoop(loopPath, file("Blank.img"));
                model.addInterfaces(loopPath, loops.interfaces(loopPath));
                return value(Variant(properdesktop::base::dbus::ObjectPath(loopPath)));
            }
            if (call.method == "CreatePartitionAndFormat")
                return value(Variant(properdesktop::base::dbus::ObjectPath(partitionPath)));
            if (call.method == "Mount")
                return value(Variant(std::string("/media/u/Blank")));
            if (call.method == "Format" && failFormat)
                return error("org.freedesktop.UDisks2.Error.Failed", "mkfs failed");
            return Result{};
        };
    }

    std::optional<BlankImageMaker::Outcome> make(BlankImage image, std::vector<BlankImageMaker::Step>* steps = nullptr)
    {
        BlankImageMaker maker(model, operations);
        std::optional<BlankImageMaker::Outcome> outcome;
        maker.onFinished = [&](const BlankImageMaker::Outcome& o) { outcome = o; };
        if (steps)
            maker.onStep = [steps](BlankImageMaker::Step step) { steps->push_back(step); };
        maker.start(std::move(image));
        for (int i = 0; i < 4; ++i)
            runPosted();
        return outcome;
    }

    BlankImage image(Scheme scheme)
    {
        return {file("Blank.img"), 64 * kMiB, "ext4", scheme, "Blank", {}};
    }

    std::string loopPath;
    std::string partitionPath;
    bool failFormat = false;
};

TEST_F(BlankImageTest, PartitionsFormatsAndMounts)
{
    std::vector<BlankImageMaker::Step> steps;
    const std::optional<BlankImageMaker::Outcome> outcome = make(image(Scheme::Gpt), &steps);
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok) << outcome->error;
    EXPECT_EQ(outcome->loop, loopPath);
    EXPECT_EQ(outcome->volume, partitionPath);
    EXPECT_EQ(outcome->mountPoint, "/media/u/Blank");
    EXPECT_EQ(std::filesystem::file_size(file("Blank.img")), 64 * kMiB);
    EXPECT_EQ(methods(backend.calls), (std::vector<std::string>{"LoopSetup Manager", "Format loop7",
                                                                "CreatePartitionAndFormat loop7", "Mount loop7p1"}));
    const Call& loopSetup = backend.calls[0];
    EXPECT_FALSE(loopSetup.arguments[1].value<VariantMap>().at("read-only").value<bool>());
    EXPECT_EQ(backend.calls[1].arguments[0].value<std::string>(), "gpt");
    const Call& create = backend.calls[2];
    EXPECT_EQ(create.arguments[0].value<uint64_t>(), kMiB);
    EXPECT_EQ(create.arguments[1].value<uint64_t>(), 0u);
    EXPECT_EQ(create.arguments[2].value<std::string>(), "0fc63daf-8483-4772-8e79-3d69d8477de4");
    EXPECT_EQ(create.arguments[3].value<std::string>(), "Blank");
    EXPECT_EQ(create.arguments[5].value<std::string>(), "ext4");
    EXPECT_EQ(create.arguments[6].value<VariantMap>().at("label").value<std::string>(), "Blank");
    EXPECT_EQ(steps, (std::vector<BlankImageMaker::Step>{
                             BlankImageMaker::Step::Creating, BlankImageMaker::Step::Attaching,
                             BlankImageMaker::Step::Partitioning, BlankImageMaker::Step::Formatting,
                             BlankImageMaker::Step::Mounting}));
}

TEST_F(BlankImageTest, MbrPartitionsHaveNoName)
{
    BlankImage mbr = image(Scheme::Mbr);
    mbr.filesystem = "vfat";
    ASSERT_TRUE(make(mbr));
    EXPECT_EQ(backend.calls[1].arguments[0].value<std::string>(), "dos");
    EXPECT_EQ(backend.calls[2].arguments[2].value<std::string>(), "0x0c");
    EXPECT_EQ(backend.calls[2].arguments[3].value<std::string>(), "");
}

TEST_F(BlankImageTest, WholeImageEncrypted)
{
    BlankImage encrypted = image(Scheme::None);
    encrypted.passphrase = "secret";
    // The format leaves the volume unlocked.
    const std::string clear = TreeBuilder::blockPath("dm_2d9");
    auto responder = backend.responder;
    backend.responder = [&, responder](const Call& call) -> std::optional<Result> {
        if (call.method == "Format") {
            TreeBuilder unlocked;
            unlocked.addDisk("loop7", {}, {.size = 64 * kMiB});
            unlocked.addCleartext("dm_2d9", loopPath, {.size = 60 * kMiB, .idUsage = "filesystem", .idType = "ext4"});
            model.addInterfaces(clear, unlocked.interfaces(clear));
            model.addInterfaces(loopPath, unlocked.interfaces(loopPath));
        }
        return responder(call);
    };
    const std::optional<BlankImageMaker::Outcome> outcome = make(encrypted);
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok);
    EXPECT_EQ(outcome->volume, clear);
    EXPECT_EQ(methods(backend.calls), (std::vector<std::string>{"LoopSetup Manager", "Format loop7", "Mount dm_2d9"}));
    const VariantMap options = backend.calls[1].arguments[1].value<VariantMap>();
    EXPECT_EQ(backend.calls[1].arguments[0].value<std::string>(), "ext4");
    EXPECT_EQ(options.at("encrypt.passphrase").value<std::string>(), "secret");
    EXPECT_EQ(options.at("label").value<std::string>(), "Blank");
}

TEST_F(BlankImageTest, FailureDetachesAndRemovesTheFile)
{
    failFormat = true;
    const std::optional<BlankImageMaker::Outcome> outcome = make(image(Scheme::Gpt));
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_EQ(outcome->error, "mkfs failed");
    EXPECT_EQ(methods(backend.calls), (std::vector<std::string>{"LoopSetup Manager", "Format loop7", "Delete loop7"}));
    EXPECT_FALSE(std::filesystem::exists(file("Blank.img")));
}

TEST_F(BlankImageTest, AnExistingFileIsKept)
{
    write("Blank.img", "keep");
    const std::optional<BlankImageMaker::Outcome> outcome = make(image(Scheme::Gpt));
    ASSERT_TRUE(outcome);
    EXPECT_FALSE(outcome->ok);
    EXPECT_NE(outcome->error, "");
    EXPECT_TRUE(backend.calls.empty());
    EXPECT_EQ(read(file("Blank.img")), "keep");
}

TEST_F(BlankImageTest, MountFailureStillMakesTheImage)
{
    auto responder = backend.responder;
    backend.responder = [responder](const Call& call) -> std::optional<Result> {
        if (call.method == "Mount")
            return error("org.freedesktop.UDisks2.Error.Failed", "no mount");
        return responder(call);
    };
    const std::optional<BlankImageMaker::Outcome> outcome = make(image(Scheme::Gpt));
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->ok);
    EXPECT_EQ(outcome->mountPoint, "");
    EXPECT_EQ(outcome->mountError, "no mount");
    EXPECT_TRUE(std::filesystem::exists(file("Blank.img")));
}

TEST_F(TransferTest, AttachedImagesAreNoSource)
{
    Model laptop;
    loadSample(laptop);
    const std::string iso = "/home/u/Downloads/debian.iso";
    // Mounted while attached, and onto its own loop device.
    EXPECT_NE(restoreProblem(laptop, Endpoint::ofFile(iso), sample::kCard).find("Eject"), std::string::npos);
    EXPECT_NE(restoreProblem(laptop, Endpoint::ofFile(iso), sample::kImage).find("itself"), std::string::npos);

    // An image attached with nothing mounted can be read.
    write("attached.img", data(1000));
    tree.setLoop(sdx, file("attached.img"));
    tree.setFilesystem(sdx1, {});
    model.reset(tree.objects());
    EXPECT_EQ(restoreProblem(model, Endpoint::ofFile(file("attached.img")), sdy), "");
    tree.setFilesystem(sdx1, {"/media/u/Small"});
    model.reset(tree.objects());
    EXPECT_NE(restoreProblem(model, Endpoint::ofFile(file("attached.img")), sdy), "");
}

TEST_F(TransferTest, FoldersOnTheSourcesFilesystem)
{
    EXPECT_TRUE(isOnFilesystemOf(model, sdx1, "/media/u/Small/image.img"));
    EXPECT_TRUE(isOnFilesystemOf(model, sdx1, "/media/u/Small/a/b/image.img"));
    EXPECT_TRUE(isOnFilesystemOf(model, sdx, "/media/u/Small/image.img"));
    EXPECT_TRUE(isOnFilesystemOf(model, sdy, "/media/u/Locked/image.img"));
    EXPECT_FALSE(isOnFilesystemOf(model, sdx1, "/media/u/Smaller/image.img"));
    EXPECT_FALSE(isOnFilesystemOf(model, sdx1, "/media/u/image.img"));
    EXPECT_FALSE(isOnFilesystemOf(model, sdy, file("image.img")));
}

TEST_F(BlankImageTest, ThePassphraseIsForgotten)
{
    BlankImage encrypted = image(Scheme::Gpt);
    encrypted.passphrase = "secret";
    BlankImageMaker maker(model, operations);
    bool finished = false;
    maker.onFinished = [&](const BlankImageMaker::Outcome&) { finished = true; };
    maker.start(encrypted);
    for (int i = 0; i < 4; ++i)
        runPosted();
    ASSERT_TRUE(finished);
    EXPECT_EQ(maker.image().passphrase, "");
    // No cleartext block appeared, so the volume was unlocked with it first.
    const std::vector<std::string> names = methods(backend.calls);
    ASSERT_GE(names.size(), 4u);
    EXPECT_EQ(names[3], "Unlock loop7p1");
    EXPECT_EQ(backend.calls[2].arguments[6].value<VariantMap>().at("encrypt.passphrase").value<std::string>(),
              "secret");
}

TEST_F(TransferTest, CompressedFilesAreNoSource)
{
    const std::vector<std::pair<std::string, std::string>> files = {
        {"xz", std::string("\xFD" "7zXZ\0\0", 8)}, {"gzip", "\x1F\x8B\x08\x00"},
        {"bzip2", "BZh91AY"},                      {"zstd", "\x28\xB5\x2F\xFD\x00"},
        {"ZIP", "PK\x03\x04\x14"},                 {"7-Zip", "7z\xBC\xAF\x27\x1C\x00"},
        {"LZ4", "\x04\x22\x4D\x18\x64"},
    };
    for (const auto& [format, head] : files) {
        const std::string path = write("compressed." + format, head + std::string(4096, 'x'));
        EXPECT_EQ(compressionOf(path), format);
        EXPECT_NE(restoreProblem(model, Endpoint::ofFile(path), sdy).find("Decompress it first"), std::string::npos)
            << format;
    }
    EXPECT_EQ(compressionOf(source), "");
    EXPECT_EQ(compressionOf(write("short", "P")), "");
    EXPECT_EQ(compressionOf(file("missing")), "");
}

TEST_F(TransferTest, AttachedImagesAreFoundThroughHardLinks)
{
    write("attached.img", data(1000));
    tree.setLoop(sdx, file("attached.img"));
    model.reset(tree.objects());
    ASSERT_EQ(::link(file("attached.img").c_str(), file("link.img").c_str()), 0);
    EXPECT_EQ(loopOfFile(model, file("link.img")), sdx);
    EXPECT_EQ(loopOfFile(model, file("attached.img")), sdx);
    write("other.img", data(1000));
    EXPECT_EQ(loopOfFile(model, file("other.img")), "");
}
