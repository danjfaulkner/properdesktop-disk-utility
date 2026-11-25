// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/rename.h"

#include "dialogs/cards.h"
#include "dialogs/volume_format.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/text_field.h"
#include "ui/window.h"

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 400;
static constexpr double kFieldWidth = 200;

RenameDialog::RenameDialog(views::Context& context, const udisks::Node& node)
    : m_context(context)
{
    const udisks::Object* content = context.model.contentOf(node.block);
    m_block = content ? content->path : node.block;
    m_type = content && content->block ? content->block->idType : std::string();
    const std::string label = content && content->block ? content->block->idLabel : std::string();

    m_identities = {udisks::identityOf(context.model, node.block)};
    if (m_block != node.block)
        m_identities.push_back(udisks::identityOf(context.model, m_block));

    m_dialog = std::make_unique<DialogWindow>(&context.window, tr("Rename Volume"), kWidth);
    // While SetLabel is out, its own answer reports a lost volume.
    m_guard = std::make_unique<IdentityGuard>(context, m_identities, udisks::displayName(context.model, node),
                                              *m_dialog, [this] { return m_waiting; });
    // A guard left after the dialog would raise its alert on a later eject.
    m_dialog->onClosed = [this] { m_guard->disarm(); };
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });

    auto* form = m_dialog->setContent<Form>();
    Card* card = form->addCard();
    m_name = card->addChild<TextFieldRow>(tr("Name"), kFieldWidth)->field();
    m_name->setName("rename:name");
    m_name->setText(label.empty() ? udisks::displayName(context.model, node) : label);
    m_name->selectAll();
    m_name->onTextChanged = [this] {
        m_error.clear();
        update();
    };
    m_name->onAccepted = [this] { rename(); };

    m_note = form->add<Footnote>();
    m_note->setName("rename:note");

    Footer* footer = form->addFooter();
    m_rename = footer->addEnd<DialogButton>(tr("Rename"), true);
    m_rename->setName("rename:rename");
    m_rename->onClicked = [this] { rename(); };
    update();
}

RenameDialog::~RenameDialog() = default;

void RenameDialog::show()
{
    m_dialog->show();
    m_name->setFocus();
}

bool RenameDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

void RenameDialog::update()
{
    const std::string problem = labelProblem(m_type, m_name->text());
    const std::string count = std::to_string(maximumLabelLength(m_type));
    std::string note = m_error.empty() ? problem : m_error;
    if (note.empty() && m_type == "vfat")
        note = format(tr("{format} names have up to {n} characters and are kept in capitals."),
                      {{"format", udisks::filesystemName(m_type)}, {"n", count}});
    else if (note.empty())
        note = format(tr("{format} names have up to {n} characters."),
                      {{"format", udisks::filesystemName(m_type)}, {"n", count}});
    m_note->setText(note);
    m_note->setError(!m_error.empty() || !problem.empty());
    m_name->setEnabled(!m_waiting);
    m_rename->setEnabled(!m_waiting && problem.empty());
}

void RenameDialog::rename()
{
    if (m_waiting || !labelProblem(m_type, m_name->text()).empty() || !m_guard->holds())
        return;
    m_waiting = true;
    update();
    m_context.operations.setLabel(m_block, normalizedLabel(m_type, m_name->text()),
                                  [this, alive = std::weak_ptr<bool>(m_alive)](const udisks::Result& result) {
                                      if (alive.expired() || !isOpen())
                                          return;
                                      m_waiting = false;
                                      if (result.ok()) {
                                          m_dialog->close();
                                          return;
                                      }
                                      if (!result.cancelled())
                                          m_error = result.errorMessage.empty() ? tr("The volume could not be renamed.")
                                                                                : result.errorMessage;
                                      update();
                                  });
}

} // namespace dialogs
