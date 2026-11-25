// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/unlock.h"

#include "dialogs/cards.h"
#include "views/context.h"

#include "udisks/errors.h"
#include "udisks/operations.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/text_field.h"
#include "ui/window.h"

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 400;
static constexpr double kFieldWidth = 200;

UnlockDialog::UnlockDialog(views::Context& context, std::string block, std::string name)
    : m_context(context)
    , m_block(std::move(block))
{
    m_dialog = std::make_unique<DialogWindow>(&context.window, tr("Unlock Volume"), kWidth);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });
    m_dialog->onClosed = [this] {
        if (onClosed)
            onClosed();
    };

    auto* form = m_dialog->setContent<Form>();
    auto* message = form->add<Label>(format(tr("Enter the passphrase to unlock “{name}”."), {{"name", name}}));
    message->setWrap(true);
    message->setName("unlock:message");

    Card* card = form->addCard();
    m_passphrase = card->addChild<TextFieldRow>(tr("Passphrase"), kFieldWidth)->field();
    m_passphrase->setName("unlock:passphrase");
    m_passphrase->setPlaceholder(tr("Required"));
    m_passphrase->setEchoMode(TextField::EchoMode::Password);
    m_passphrase->onTextChanged = [this] { update(); };
    m_passphrase->onAccepted = [this] { unlock(); };

    m_error = form->add<Footnote>();
    m_error->setName("unlock:error");
    m_error->setError(true);

    Footer* footer = form->addFooter();
    m_unlock = footer->addEnd<DialogButton>(tr("Unlock"), true);
    m_unlock->setName("unlock:unlock");
    m_unlock->onClicked = [this] { unlock(); };
    update();
}

UnlockDialog::~UnlockDialog() = default;

void UnlockDialog::show()
{
    m_dialog->show();
    m_passphrase->setFocus();
}

bool UnlockDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

void UnlockDialog::update()
{
    m_passphrase->setEnabled(!m_waiting);
    m_unlock->setEnabled(!m_waiting && !m_passphrase->text().empty());
}

void UnlockDialog::unlock()
{
    if (m_waiting || m_passphrase->text().empty())
        return;
    m_waiting = true;
    m_error->setText({});
    update();
    m_context.operations.unlock(m_block, m_passphrase->text(),
                                [this, alive = std::weak_ptr<bool>(m_alive)](const udisks::Result& result) {
                                    if (alive.expired() || !isOpen())
                                        return;
                                    m_waiting = false;
                                    if (result.ok()) {
                                        // The passphrase stays in memory no longer than it is needed.
                                        m_passphrase->setText({});
                                        if (onUnlocked)
                                            onUnlocked(result.objectPath());
                                        m_dialog->close();
                                        return;
                                    }
                                    if (udisks::isWrongPassphrase(result.errorName, result.errorMessage)) {
                                        m_error->setText(tr("The passphrase is incorrect."));
                                        m_passphrase->setText({});
                                    } else if (!result.cancelled()) {
                                        m_error->setText(result.errorMessage.empty()
                                                                 ? tr("The volume could not be unlocked.")
                                                                 : result.errorMessage);
                                    }
                                    update();
                                    m_passphrase->setFocus();
                                });
}

} // namespace dialogs
