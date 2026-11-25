// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "images/progress.h"
#include "images/transfer.h"

#include <functional>
#include <memory>
#include <string>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class Label;
class ProgressBar;
}

namespace views {
struct Context;
}

namespace dialogs {

class Footnote;

// A copy onto an image file or a disk, while it runs: what is copied, a
// bar, the amount, speed and time left, and Stop. A finished copy closes
// the dialog with a message over the window; a failed one stays with its
// error until closed.
class CopyDialog
{
public:
    CopyDialog(views::Context& context, std::string title, std::string message, std::string doneMessage);
    ~CopyDialog();

    CopyDialog(const CopyDialog&) = delete;
    CopyDialog& operator=(const CopyDialog&) = delete;

    void start(images::Endpoint source, images::Endpoint target);
    bool isRunning() const { return m_transfer.isRunning(); }
    bool isOpen() const;
    images::Transfer& transfer() { return m_transfer; }

    std::function<void(const images::Transfer::Outcome& outcome)> onFinished;

private:
    void step(images::Transfer::Step step);
    void progress(uint64_t copied, uint64_t total);
    void finished(const images::Transfer::Outcome& outcome);

    views::Context& m_context;
    std::string m_doneMessage;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    properdesktop::ui::ProgressBar* m_bar;
    properdesktop::ui::Label* m_status;
    properdesktop::ui::Label* m_timeLeft;
    Footnote* m_error;
    properdesktop::ui::DialogButton* m_stop;
    images::Transfer m_transfer;
    images::RateMeter m_meter;
    bool m_ontoBlock = false;
    // Writing has begun: a block target stopped now is no longer what it was.
    bool m_copying = false;
};

} // namespace dialogs
