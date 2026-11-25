// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/diskimages.h"
#include "dialogs/identity_guard.h"
#include "dialogs/copyprogress.h"
#include "dialogs/newimage.h"
#include "dialogs/restore.h"

#include "application.h"
#include "images/imagefile.h"
#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "views/context.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "ui/dialog_window.h"
#include "ui/file_chooser.h"

#include <algorithm>
#include <filesystem>

#include <unistd.h>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr const char* kReadOnlyChoice = "readOnly";

DiskImages::DiskImages(views::Context& context)
    : m_context(context)
{
    ActionDispatcher& actions = context.actions;
    actions.setHandler(Action::AttachImage, [this](const Selection&) { attachImage(); });
    actions.setHandler(Action::NewBlankImage, [this](const Selection&) {
        if (!m_newImage) {
            m_newImage = std::make_unique<NewImageDialog>(
                    m_context, [this](const std::string& current, std::function<void(const std::string&)> done) {
                        chooseFile({FileRequest::Kind::Folder, {}, current},
                                   [done](const std::string& path, bool) { done(path); });
                    });
            m_newImage->onCreated = [this](const images::BlankImageMaker::Outcome& outcome) {
                selectWhenListed(outcome.loop);
                if (!outcome.mountError.empty())
                    m_context.showError(tr("The new image could not be mounted."), outcome.mountError);
            };
        }
        m_newImage->show();
    });
    actions.setHandler(Action::NewImageFromSelection,
                       [this](const Selection& selection) { newImageFromSelection(selection); });
    actions.setHandler(Action::Restore, [this](const Selection& selection) {
        if (!selection)
            return;
        if (!m_restore) {
            m_restore = std::make_unique<RestoreDialog>(
                    m_context, [this](std::function<void(const std::string&)> done) {
                        chooseFile({FileRequest::Kind::ChooseImage, {}, {}},
                                   [done](const std::string& path, bool) { done(path); });
                    });
            m_restore->onRestore = [this](const images::Endpoint& source, const std::string& sourceName,
                                          const udisks::Node& target, const std::string& targetName) {
                startCopy(format(tr("Restoring “{name}”"), {{"name", targetName}}),
                          format(tr("Copying “{source}” onto “{target}”."),
                                 {{"source", sourceName}, {"target", targetName}}),
                          format(tr("“{name}” was restored."), {{"name", targetName}}), source,
                          images::Endpoint::ofBlock(target.block));
            };
        }
        m_restore->show(*selection);
    });

    for (Action action : {Action::Restore, Action::NewImageFromSelection}) {
        actions.setRefinement(action, [this](const Selection&) {
            if (isCopying())
                return ActionState{false, tr("A copy is in progress.")};
            return ActionState{true, {}};
        });
    }

    m_subscriptions.push_back(context.treeChanged.connect([this] { selectPending(); }));
    m_subscriptions.push_back(context.application.openRequested.connect(
        [this](const std::vector<std::string>& paths) { open(paths); }));
}

DiskImages::~DiskImages()
{
    // The window's actions outlive this.
    for (Action action : {Action::AttachImage, Action::NewBlankImage, Action::NewImageFromSelection, Action::Restore})
        m_context.actions.setHandler(action, {});
    for (Action action : {Action::Restore, Action::NewImageFromSelection})
        m_context.actions.setRefinement(action, {});
}

void DiskImages::chooseFile(const FileRequest& request, FileChosen chosen)
{
    if (m_fileDialog) {
        m_fileDialog(request, std::move(chosen));
        return;
    }
    m_chooser = std::make_unique<FileChooser>(&m_context.window);
    FileChooser::Filter imageFiles;
    imageFiles.name = tr("Disk Images");
    imageFiles.patterns = images::imagePatterns();
    FileChooser::Filter all;
    all.name = tr("All Files");
    all.patterns = {"*"};
    switch (request.kind) {
    case FileRequest::Kind::OpenImage:
        m_chooser->setMode(FileChooser::Mode::OpenFile);
        m_chooser->setTitle(tr("Open Disk Image"));
        m_chooser->setAcceptLabel(tr("Open"));
        m_chooser->setFilters({imageFiles, all});
        m_chooser->setChoices({{kReadOnlyChoice, tr("Open read-only"), {}, "false"}});
        break;
    case FileRequest::Kind::ChooseImage:
        m_chooser->setMode(FileChooser::Mode::OpenFile);
        m_chooser->setTitle(tr("Restore from Disk Image"));
        m_chooser->setAcceptLabel(tr("Choose"));
        m_chooser->setFilters({imageFiles, all});
        break;
    case FileRequest::Kind::SaveImage: {
        m_chooser->setMode(FileChooser::Mode::SaveFile);
        m_chooser->setTitle(tr("Save Image As"));
        m_chooser->setAcceptLabel(tr("Save"));
        FileChooser::Filter raw;
        raw.name = tr("Disk Images");
        raw.patterns = {"*.img"};
        m_chooser->setFilters({raw});
        if (!request.folder.empty()) {
            m_chooser->setCurrentFolder(request.folder);
            m_chooser->setCurrentFile((std::filesystem::path(request.folder) / request.suggested).string());
        } else {
            m_chooser->setCurrentFile(request.suggested);
        }
        break;
    }
    case FileRequest::Kind::Folder:
        m_chooser->setMode(FileChooser::Mode::OpenFolder);
        m_chooser->setTitle(tr("Choose a Folder"));
        m_chooser->setAcceptLabel(tr("Choose"));
        if (!request.folder.empty())
            m_chooser->setCurrentFolder(request.folder);
        break;
    }
    FileChooser* chooser = m_chooser.get();
    m_chooser->onAccepted = [chooser, chosen = std::move(chosen)](const std::vector<std::string>& paths,
                                                                  const std::string&) {
        if (paths.empty())
            return;
        const auto& options = chooser->chosenOptions();
        const bool readOnly = std::any_of(options.begin(), options.end(), [](const auto& option) {
            return option.first == kReadOnlyChoice && option.second == "true";
        });
        chosen(paths.front(), readOnly);
    };
    m_chooser->open();
}

void DiskImages::attachImage()
{
    chooseFile({FileRequest::Kind::OpenImage, {}, {}},
               [this](const std::string& path, bool readOnly) { open({path}, readOnly); });
}

void DiskImages::open(const std::vector<std::string>& paths, bool readOnly)
{
    for (const std::string& path : paths) {
        if (const std::string loop = images::loopOfFile(m_context.model, path); !loop.empty()) {
            selectWhenListed(loop);
            continue;
        }
        const bool attachReadOnly = readOnly || images::attachesReadOnly(path) || ::access(path.c_str(), W_OK) != 0;
        const std::string name = std::filesystem::path(path).filename().string();
        m_context.operations.loopSetup(
                path, attachReadOnly,
                [this, name, alive = std::weak_ptr<bool>(m_alive)](const udisks::Result& result) {
                    if (alive.expired())
                        return;
                    if (result.ok() && !result.objectPath().empty()) {
                        selectWhenListed(result.objectPath());
                        return;
                    }
                    if (!result.cancelled())
                        m_context.showError(format(tr("“{name}” could not be opened."), {{"name", name}}),
                                            result.errorMessage);
                });
    }
}

// The node to select for a disk: the first the sidebar lists on it, the
// disk itself with every device shown, else its first volume.
static const udisks::Node* firstOn(const std::vector<udisks::Node>& nodes, const std::string& disk)
{
    for (const udisks::Node& node : nodes) {
        if (node.disk == disk)
            return &node;
        if (const udisks::Node* found = firstOn(node.children, disk))
            return found;
    }
    return nullptr;
}

void DiskImages::selectWhenListed(const std::string& disk)
{
    m_pendingSelection = disk;
    selectPending();
}

void DiskImages::selectPending()
{
    if (m_pendingSelection.empty())
        return;
    for (const udisks::SectionNodes& section : m_context.tree) {
        if (const udisks::Node* node = firstOn(section.nodes, m_pendingSelection)) {
            m_pendingSelection.clear();
            m_context.select(node->id);
            return;
        }
    }
}

// A name for a file: no slashes, and not hidden.
static std::string fileNameFor(std::string name)
{
    std::replace(name.begin(), name.end(), '/', '-');
    while (!name.empty() && name.front() == '.')
        name.erase(name.begin());
    return (name.empty() ? std::string(tr("Untitled")) : name) + ".img";
}

void DiskImages::newImageFromSelection(const Selection& selection)
{
    if (!selection)
        return;
    const udisks::Node node = *selection;
    const std::string name = udisks::displayName(m_context.model, node);
    std::string folder = properdesktop::base::paths::userDirectory(properdesktop::base::paths::UserDirectory::Documents);
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error))
        folder = properdesktop::base::paths::home();
    chooseFile({FileRequest::Kind::SaveImage, fileNameFor(name), folder},
               [this, node, name](const std::string& path, bool) { makeImage(node, name, path); });
}

void DiskImages::makeImage(const udisks::Node& node, const std::string& name, const std::string& path)
{
    const udisks::Model& model = m_context.model;
    const std::string title = tr("The image could not be created.");
    if (!model.object(node.block))
        return;
    if (!images::loopOfFile(model, path).empty()) {
        m_context.showError(title, tr("The file is an attached disk image. Eject it first."));
        return;
    }
    // The folder goes away with the unmount.
    if (images::isOnFilesystemOf(model, node.block, path)) {
        m_context.showError(title, format(tr("The image cannot be saved on “{name}” itself. Choose a folder on "
                                             "another disk."),
                                          {{"name", name}}));
        return;
    }
    const std::vector<std::string> mounted = images::mountedOn(model, node.block);
    const std::vector<std::string> unlocked = images::unlockedOn(model, node.block);
    if ((!mounted.empty() || !unlocked.empty()) && model.holdsSystem(node.block)) {
        m_context.showError(title, format(tr("“{name}” holds the running system, so it cannot be unmounted to "
                                             "be read."),
                                          {{"name", name}}));
        return;
    }

    const std::string fileName = std::filesystem::path(path).filename().string();
    // Taken now: a stick swapped while the question is open is not read.
    const images::Endpoint source = images::Endpoint::ofBlock(model, node.block);
    const auto start = [this, source, name, path, fileName] {
        startCopy(format(tr("Creating “{name}”"), {{"name", fileName}}),
                  format(tr("Copying “{source}” into “{file}”."), {{"source", name}, {"file", fileName}}),
                  format(tr("“{file}” was created."), {{"file", fileName}}), source, images::Endpoint::ofFile(path));
    };
    if (mounted.empty() && unlocked.empty()) {
        start();
        return;
    }
    // A mounted filesystem changes while it is read.
    m_confirm = std::make_unique<ConfirmDialog>(
            &m_context.window, format(tr("“{name}” must be unmounted to make an image of it."), {{"name", name}}),
            tr("It is unmounted, and locked when it is encrypted, before it is read, and stays so afterwards."));
    m_confirm->setConfirmText(tr("Unmount"));
    m_confirm->onAccepted = [this, start] {
        m_confirmGuard->disarm();
        start();
    };
    m_confirm->onRejected = [this] { m_confirmGuard->disarm(); };
    // The question goes with the volume it asks about.
    m_confirmGuard = std::make_unique<IdentityGuard>(
            m_context, std::vector<udisks::Identity>{source.identity}, name, [this] {
                if (!m_confirm)
                    return;
                // Its window may be handling the event that got us here.
                std::shared_ptr<ConfirmDialog> confirm(std::move(m_confirm));
                properdesktop::base::EventLoop::current()->post([confirm] {});
            });
    m_confirm->show();
}

void DiskImages::startCopy(std::string title, std::string message, std::string doneMessage, images::Endpoint source,
                           images::Endpoint target)
{
    // Replacing the running copy would stop it and remove its image file.
    if (isCopying()) {
        m_context.showError(tr("Another copy is in progress."), tr("Wait for it to finish, or stop it first."));
        return;
    }
    m_copy = std::make_unique<CopyDialog>(m_context, std::move(title), std::move(message), std::move(doneMessage));
    m_copy->onFinished = [this](const images::Transfer::Outcome&) { m_context.actions.refresh(); };
    m_copy->start(std::move(source), std::move(target));
    m_context.actions.refresh();
}

bool DiskImages::isCopying() const
{
    return m_copy && m_copy->isRunning();
}

} // namespace dialogs
