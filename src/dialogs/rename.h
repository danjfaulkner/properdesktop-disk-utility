// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "dialogs/identity_guard.h"

#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class TextField;
}

namespace views {
struct Context;
}

namespace dialogs {

class Footnote;

// A volume's new name, within what its filesystem stores: the limit shows
// under the field, the name goes in capitals on FAT.
class RenameDialog
{
public:
    RenameDialog(views::Context& context, const udisks::Node& node);
    ~RenameDialog();

    RenameDialog(const RenameDialog&) = delete;
    RenameDialog& operator=(const RenameDialog&) = delete;

    void show();
    bool isOpen() const;

private:
    void update();
    void rename();

    views::Context& m_context;
    // The block that carries the filesystem: the cleartext one when encrypted.
    std::string m_block;
    std::string m_type;
    // The volume and that block as the dialog opened on them.
    std::vector<udisks::Identity> m_identities;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<IdentityGuard> m_guard;
    properdesktop::ui::TextField* m_name;
    properdesktop::ui::DialogButton* m_rename;
    Footnote* m_note;
    bool m_waiting = false;
    std::string m_error;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
