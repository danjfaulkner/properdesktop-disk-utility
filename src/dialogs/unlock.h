// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>

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

// The passphrase of an encrypted volume. Unlock stays on screen until
// UDisks2 answers: a wrong passphrase is said under the field, a dismissed
// password prompt leaves the dialog as it was.
class UnlockDialog
{
public:
    UnlockDialog(views::Context& context, std::string block, std::string name);
    ~UnlockDialog();

    UnlockDialog(const UnlockDialog&) = delete;
    UnlockDialog& operator=(const UnlockDialog&) = delete;

    void show();
    bool isOpen() const;

    // With the cleartext block, before the dialog closes.
    std::function<void(const std::string& cleartext)> onUnlocked;
    // Unlocked or not.
    std::function<void()> onClosed;

private:
    void unlock();
    void update();

    views::Context& m_context;
    std::string m_block;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    properdesktop::ui::TextField* m_passphrase;
    properdesktop::ui::DialogButton* m_unlock;
    Footnote* m_error;
    bool m_waiting = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
