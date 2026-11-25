// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/copyprogress.h"
#include "dialogs/cards.h"

#include "udisks/model.h"
#include "views/context.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/progress_bar.h"
#include "ui/window.h"

using namespace properdesktop::ui;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 420;

CopyDialog::CopyDialog(views::Context& context, std::string title, std::string message, std::string doneMessage)
    : m_context(context)
    , m_doneMessage(std::move(doneMessage))
    , m_dialog(std::make_unique<DialogWindow>(&context.window, std::move(title), kWidth))
    , m_transfer(context.model, context.operations)
{
    auto* form = m_dialog->setContent<Form>();
    auto* text = form->add<Label>(std::move(message));
    text->setWrap(true);
    m_bar = form->add<ProgressBar>();
    m_bar->setName("copy:progress");
    m_bar->setIndeterminate(true);
    m_status = form->add<Label>(tr("Preparing…"));
    m_status->setName("copy:status");
    m_status->setColor(ColorId::DisabledText);
    m_timeLeft = form->add<Label>();
    m_timeLeft->setName("copy:timeLeft");
    m_timeLeft->setColor(ColorId::DisabledText);
    m_error = form->add<Footnote>();
    m_error->setName("copy:error");
    m_error->setError(true);
    Footer* footer = form->addFooter();
    m_stop = footer->addEnd<DialogButton>(tr("Stop"));
    m_stop->setName("copy:stop");
    m_stop->onClicked = [this] {
        m_stop->setEnabled(false);
        m_status->setText(tr("Stopping…"));
        m_transfer.cancel();
    };

    // The copy cannot be left running behind a closed dialog: Stop ends it.
    m_dialog->setCloseVisible(false);
    DialogWindow* dialog = m_dialog.get();
    m_dialog->window()->onCloseRequested = [this, dialog] {
        if (!m_transfer.isRunning())
            dialog->close();
    };
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this, dialog] {
        if (!m_transfer.isRunning())
            dialog->close();
    });

    m_transfer.onStep = [this](images::Transfer::Step step) { this->step(step); };
    m_transfer.onProgress = [this](uint64_t copied, uint64_t total) { progress(copied, total); };
    m_transfer.onFinished = [this](const images::Transfer::Outcome& outcome) { finished(outcome); };
}

CopyDialog::~CopyDialog() = default;

bool CopyDialog::isOpen() const
{
    return m_dialog && m_dialog->window();
}

void CopyDialog::start(images::Endpoint source, images::Endpoint target)
{
    m_ontoBlock = target.isBlock();
    m_copying = false;
    m_dialog->show();
    m_transfer.start(std::move(source), std::move(target));
}

void CopyDialog::step(images::Transfer::Step step)
{
    switch (step) {
    case images::Transfer::Step::Unmounting:
        m_status->setText(tr("Unmounting…"));
        break;
    case images::Transfer::Step::Opening:
        m_status->setText(tr("Opening…"));
        break;
    case images::Transfer::Step::Copying:
        m_copying = true;
        m_meter.start(images::RateMeter::Clock::now());
        m_bar->setIndeterminate(false);
        m_bar->setValue(0);
        m_status->setText(images::progressText(0, m_transfer.total(), 0));
        break;
    }
}

void CopyDialog::progress(uint64_t copied, uint64_t total)
{
    m_meter.add(images::RateMeter::Clock::now(), copied);
    m_bar->setValue(total ? double(copied) / double(total) : 0);
    if (!m_stop->isEnabled())
        return;
    m_status->setText(images::progressText(copied, total, m_meter.rate()));
    const std::optional<std::chrono::seconds> remaining = m_meter.remaining(total);
    m_timeLeft->setText(remaining ? images::timeLeftText(*remaining) : std::string());
}

void CopyDialog::finished(const images::Transfer::Outcome& outcome)
{
    const auto done = onFinished;
    const bool overwritten = m_ontoBlock && m_copying;
    if (outcome.ok || (outcome.cancelled && !overwritten)) {
        m_dialog->close();
        if (outcome.ok)
            m_context.toast(m_doneMessage);
        else if (m_copying)
            m_context.toast(tr("Stopped. The incomplete image file was removed."));
        else
            m_context.toast(tr("Stopped."));
    } else {
        // What is left behind matters more than why it stopped.
        std::string text = outcome.cancelled ? tr("Stopped.") : outcome.error;
        if (overwritten)
            text += " " + std::string(tr("The destination was overwritten in part. Restore or erase it again before "
                                         "you use it."));
        else if (m_copying && !m_ontoBlock)
            text += " " + std::string(tr("The incomplete image file was removed."));
        m_bar->setVisible(false);
        m_timeLeft->setText({});
        m_status->setText({});
        m_status->setVisible(false);
        m_error->setText(text);
        m_stop->setVisible(false);
        m_dialog->setCloseVisible(true);
    }
    if (done)
        done(outcome);
}

} // namespace dialogs
