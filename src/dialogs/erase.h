// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "dialogs/erase_plan.h"
#include "dialogs/identity_guard.h"
#include "dialogs/volume_format.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class Label;
class Menu;
class ProgressBar;
class Switch;
class TextField;
}

namespace views {
struct Context;
}

namespace dialogs {

class ChoiceButton;
class Footnote;
class PlanRunner;
class TextFieldRow;

// Erases a volume, a partition or a whole disk: a name, a format, optional
// encryption and, for a whole disk, the partition scheme. While it runs the
// dialog shows each step and the job's progress, then the outcome.
class EraseDialog
{
public:
    EraseDialog(views::Context& context, udisks::Node node);
    ~EraseDialog();

    EraseDialog(const EraseDialog&) = delete;
    EraseDialog& operator=(const EraseDialog&) = delete;

    void show();
    bool isOpen() const;
    bool isRunning() const;

    // What the form asks for now.
    EraseRequest request() const;
    // Why Erase cannot go ahead; empty when it can.
    std::string problem() const;

private:
    void buildForm();
    void updateFormats();
    void update();
    void showSecurityOptions();
    void erase();
    void showProgress();
    void stepStarted(size_t index);
    void updateProgress();
    void finished();
    void stop();
    // The form as the last erase was asked for.
    void restore();
    // The views of the content shown, which goes when it is replaced or the
    // window closes.
    void forgetViews();

    views::Context& m_context;
    udisks::Node m_node;
    // The block as the dialog opened on it, checked again before anything.
    udisks::Identity m_identity;
    std::string m_name;
    uint64_t m_size = 0;
    std::string m_defaultFormat;
    FormatAvailability m_formats;
    bool m_zeroFill = false;
    bool m_stopped = false;
    std::optional<EraseRequest> m_lastRequest;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<PlanRunner> m_runner;
    std::unique_ptr<IdentityGuard> m_guard;
    std::unique_ptr<properdesktop::ui::Menu> m_securityMenu;

    properdesktop::ui::TextField* m_label = nullptr;
    ChoiceButton* m_format = nullptr;
    ChoiceButton* m_scheme = nullptr;
    Footnote* m_schemeNote = nullptr;
    properdesktop::ui::Switch* m_encrypt = nullptr;
    TextFieldRow* m_passphraseRow = nullptr;
    TextFieldRow* m_verifyRow = nullptr;
    Footnote* m_securityNote = nullptr;
    Footnote* m_problem = nullptr;
    properdesktop::ui::DialogButton* m_eraseButton = nullptr;
    properdesktop::ui::DialogButton* m_securityButton = nullptr;

    properdesktop::ui::Label* m_status = nullptr;
    properdesktop::ui::ProgressBar* m_bar = nullptr;
    Footnote* m_stepNote = nullptr;
    Footnote* m_error = nullptr;
    properdesktop::ui::DialogButton* m_done = nullptr;
    properdesktop::ui::DialogButton* m_stop = nullptr;

    std::vector<properdesktop::base::Subscription> m_subscriptions;
};

} // namespace dialogs
