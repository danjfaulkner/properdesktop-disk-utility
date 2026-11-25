// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/identity.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogWindow;
}

namespace views {
struct Context;
}

namespace dialogs {

// A dialog's hold on the blocks it was opened on: once one of them is gone,
// or another disk or image took its path, the dialog is closed at once and
// the window says why. Checked on every change of the model, until the
// guard is disarmed.
class IdentityGuard
{
public:
    // Calls close on the loss, whatever the dialog is doing.
    IdentityGuard(views::Context& context, std::vector<udisks::Identity> identities, std::string name,
                  std::function<void()> close);
    // Closes the dialog on the loss, except while busy() says a run is under
    // way: the run stops before its next step and reports the loss itself,
    // and the dialog stays to show it. Once busy() ends, the blocks are
    // checked again, and the dialog is closed then if they are still gone.
    // Disarms itself once the dialog has closed, so a later loss says
    // nothing.
    IdentityGuard(views::Context& context, std::vector<udisks::Identity> identities, std::string name,
                  properdesktop::ui::DialogWindow& dialog, std::function<bool()> busy = {});

    ~IdentityGuard();

    IdentityGuard(const IdentityGuard&) = delete;
    IdentityGuard& operator=(const IdentityGuard&) = delete;

    // Every block is still the one it was.
    bool holds() const;
    // Watches no more: the confirmation it guarded was answered, or its
    // dialog closed. Safe from the guard's own callback.
    void disarm();
    bool isArmed() const { return m_watch != nullptr; }

private:
    void lost();
    // Closes the dialog and says why.
    void close();
    void watch();
    void waitUntilIdle();
    void stopWaiting();

    views::Context& m_context;
    std::string m_name;
    std::function<void()> m_close;
    properdesktop::ui::DialogWindow* m_dialog = nullptr;
    std::function<bool()> m_busy;
    std::vector<udisks::Identity> m_identities;
    std::unique_ptr<udisks::IdentityWatch> m_watch;
    // Polls busy() after a loss while busy; -1 while not waiting.
    int m_idleTimer = -1;
};

// The window's alert for a block that went while a dialog was open on it.
void showGone(views::Context& context, const std::string& name);

} // namespace dialogs
