// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/identity_guard.h"

#include "views/context.h"

#include "udisks/model.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "ui/dialog_window.h"

using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

void showGone(views::Context& context, const std::string& name)
{
    if (context.showError)
        context.showError(format(tr("“{name}” is no longer there."), {{"name", name}}),
                          tr("It was removed or replaced by another disk, so nothing more is done to it."));
}

IdentityGuard::IdentityGuard(views::Context& context, std::vector<udisks::Identity> identities, std::string name,
                             std::function<void()> close)
    : m_context(context)
    , m_name(std::move(name))
    , m_close(std::move(close))
    , m_identities(std::move(identities))
{
    watch();
}

IdentityGuard::IdentityGuard(views::Context& context, std::vector<udisks::Identity> identities, std::string name,
                             properdesktop::ui::DialogWindow& dialog, std::function<bool()> busy)
    : m_context(context)
    , m_name(std::move(name))
    , m_dialog(&dialog)
    , m_busy(std::move(busy))
    , m_identities(std::move(identities))
{
    watch();
}

IdentityGuard::~IdentityGuard()
{
    stopWaiting();
}

void IdentityGuard::watch()
{
    m_watch = std::make_unique<udisks::IdentityWatch>(m_context.model, m_identities, [this] { lost(); });
}

bool IdentityGuard::holds() const
{
    for (const udisks::Identity& identity : m_identities) {
        if (!m_context.model.matches(identity))
            return false;
    }
    return true;
}

void IdentityGuard::disarm()
{
    stopWaiting();
    m_watch.reset();
}

void IdentityGuard::stopWaiting()
{
    if (m_idleTimer < 0)
        return;
    if (properdesktop::base::EventLoop* loop = properdesktop::base::EventLoop::current())
        loop->stopTimer(m_idleTimer);
    m_idleTimer = -1;
}

// The watch calls back once; a run that was busy then may end without the
// model changing again, such as a call turned down at the password.
void IdentityGuard::waitUntilIdle()
{
    if (m_idleTimer >= 0)
        return;
    m_idleTimer = properdesktop::base::EventLoop::current()->startTimer(std::chrono::milliseconds(50), true, [this] {
        if (m_busy && m_busy())
            return;
        stopWaiting();
        if (!m_dialog->window()) {
            disarm();
            return;
        }
        if (holds())
            watch();
        else
            close();
    });
}

void IdentityGuard::lost()
{
    if (m_dialog) {
        // A dialog closed by its own Done or Escape has nothing left to guard.
        if (!m_dialog->window()) {
            disarm();
            return;
        }
        if (m_busy && m_busy()) {
            waitUntilIdle();
            return;
        }
    }
    close();
}

void IdentityGuard::close()
{
    disarm();
    if (m_dialog) {
        m_dialog->close();
        showGone(m_context, m_name);
        return;
    }
    // The callback may destroy the guard.
    const std::function<void()> close = m_close;
    const std::string name = m_name;
    views::Context& context = m_context;
    if (close)
        close();
    showGone(context, name);
}

} // namespace dialogs
