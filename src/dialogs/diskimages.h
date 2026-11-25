// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "images/transfer.h"

#include "base/signal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class ConfirmDialog;
class FileChooser;
}

namespace views {
struct Context;
}

namespace dialogs {

class CopyDialog;
class IdentityGuard;
class NewImageDialog;
class RestoreDialog;

// The disk image actions: Open Disk Image, New Blank Image, New Image from
// the selection and Restore, set on the window's actions; and the images
// another launch or Files hands over, which are attached and selected.
class DiskImages
{
public:
    // What the file dialog is asked for.
    struct FileRequest
    {
        enum class Kind {
            // An image to attach, with a read-only choice.
            OpenImage,
            // An image to restore from.
            ChooseImage,
            // Where to save an image; suggested is its file name.
            SaveImage,
            Folder,
        };
        Kind kind = Kind::OpenImage;
        std::string suggested;
        std::string folder;
    };
    // The path chosen, and for OpenImage the read-only choice; not called
    // when the dialog is cancelled.
    using FileChosen = std::function<void(const std::string& path, bool readOnly)>;
    using FileDialog = std::function<void(const FileRequest& request, FileChosen chosen)>;

    explicit DiskImages(views::Context& context);
    ~DiskImages();

    DiskImages(const DiskImages&) = delete;
    DiskImages& operator=(const DiskImages&) = delete;

    // Attaches each image, or selects it when it is attached already, and
    // selects the last once it is listed. An optical disc image, or one the
    // user cannot write, attaches read-only whatever readOnly says.
    void open(const std::vector<std::string>& paths, bool readOnly = false);
    // In place of the desktop's file dialog, for tests.
    void setFileDialog(FileDialog dialog) { m_fileDialog = std::move(dialog); }

    NewImageDialog* newImageDialog() const { return m_newImage.get(); }
    RestoreDialog* restoreDialog() const { return m_restore.get(); }
    CopyDialog* copyDialog() const { return m_copy.get(); }
    properdesktop::ui::ConfirmDialog* confirmation() const { return m_confirm.get(); }
    // A copy onto an image file or a disk is running.
    bool isCopying() const;

private:
    void attachImage();
    void newImageFromSelection(const Selection& selection);
    void makeImage(const udisks::Node& node, const std::string& name, const std::string& path);
    void startCopy(std::string title, std::string message, std::string doneMessage, images::Endpoint source,
                   images::Endpoint target);
    void selectWhenListed(const std::string& disk);
    void selectPending();
    void chooseFile(const FileRequest& request, FileChosen chosen);

    views::Context& m_context;
    FileDialog m_fileDialog;
    std::unique_ptr<properdesktop::ui::FileChooser> m_chooser;
    std::unique_ptr<NewImageDialog> m_newImage;
    std::unique_ptr<RestoreDialog> m_restore;
    std::unique_ptr<CopyDialog> m_copy;
    std::unique_ptr<properdesktop::ui::ConfirmDialog> m_confirm;
    std::unique_ptr<IdentityGuard> m_confirmGuard;
    // The disk to select once the sidebar lists it.
    std::string m_pendingSelection;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
