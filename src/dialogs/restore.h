// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "dialogs/identity_guard.h"
#include "images/transfer.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class ConfirmDialog;
class DialogButton;
class DialogWindow;
}

namespace views {
struct Context;
}

namespace dialogs {

class ChoiceButton;
class Footnote;

// Restore: the volume, disk or image file to copy onto the selection,
// which is erased by it. Restore asks once more, naming what is lost,
// before onRestore starts the copy.
class RestoreDialog
{
public:
    // Asks for an image file; done with its path, or not at all.
    using ChooseImage = std::function<void(std::function<void(const std::string& path)> done)>;

    struct Source
    {
        // In the menu, with the device and size; the name alone.
        std::string title;
        std::string name;
        images::Endpoint endpoint;
    };

    RestoreDialog(views::Context& context, ChooseImage chooseImage);
    ~RestoreDialog();

    RestoreDialog(const RestoreDialog&) = delete;
    RestoreDialog& operator=(const RestoreDialog&) = delete;

    void show(const udisks::Node& target);
    bool isOpen() const;

    const std::vector<Source>& sources() const { return m_sources; }
    // Adds the image to the sources, first, and chooses it.
    void setImage(const std::string& path);
    // Asks to confirm, then calls onRestore.
    void restore();
    properdesktop::ui::ConfirmDialog* confirmation() const { return m_confirm.get(); }

    std::function<void(const images::Endpoint& source, const std::string& sourceName, const udisks::Node& target,
                       const std::string& targetName)>
        onRestore;

private:
    void build();
    // Closes the confirmation once the event being handled is over.
    void dropConfirmation();
    void reloadSources();
    void update();
    const Source* chosen() const;

    views::Context& m_context;
    ChooseImage m_chooseImage;
    udisks::Node m_target;
    std::string m_targetName;
    // The target as the dialog opened on it.
    udisks::Identity m_targetIdentity;
    std::string m_image;
    // Why nothing is chosen: the source chosen went away. Kept until the
    // user picks a source, whatever the model does meanwhile.
    std::string m_removed;
    bool m_nothingChosen = false;
    std::vector<Source> m_sources;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<properdesktop::ui::ConfirmDialog> m_confirm;
    std::unique_ptr<IdentityGuard> m_guard;
    ChoiceButton* m_source = nullptr;
    Footnote* m_problem = nullptr;
    properdesktop::ui::DialogButton* m_restore = nullptr;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
};

} // namespace dialogs
