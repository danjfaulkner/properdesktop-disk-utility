// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/restore.h"
#include "dialogs/cards.h"
#include "dialogs/choice_button.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "views/context.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "ui/button.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/window.h"

#include <algorithm>
#include <filesystem>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 460;

RestoreDialog::RestoreDialog(views::Context& context, ChooseImage chooseImage)
    : m_context(context)
    , m_chooseImage(std::move(chooseImage))
{
    m_subscriptions.push_back(context.treeChanged.connect([this] {
        if (!isOpen())
            return;
        if (!m_context.model.object(m_target.block)) {
            m_dialog->close();
            return;
        }
        reloadSources();
    }));
}

RestoreDialog::~RestoreDialog() = default;

bool RestoreDialog::isOpen() const
{
    return m_dialog && m_dialog->window();
}

void RestoreDialog::show(const udisks::Node& target)
{
    if (isOpen() && target.block == m_target.block) {
        m_dialog->window()->activate();
        return;
    }
    // A confirmation still open was about the previous target.
    dropConfirmation();
    m_target = target;
    m_targetName = udisks::displayName(m_context.model, target);
    m_targetIdentity = udisks::identityOf(m_context.model, target.block);
    m_image.clear();
    m_nothingChosen = false;
    m_removed.clear();
    // The old guard points at the dialog build() replaces.
    m_guard.reset();
    build();
    // The question goes with the dialog, which onClosed drops.
    m_guard = std::make_unique<IdentityGuard>(m_context, std::vector<udisks::Identity>{m_targetIdentity},
                                              m_targetName, *m_dialog);
    reloadSources();
    m_dialog->show();
}

void RestoreDialog::dropConfirmation()
{
    if (!m_confirm)
        return;
    // Its window and callback may be running the click that brought us here.
    std::shared_ptr<ConfirmDialog> confirm(std::move(m_confirm));
    properdesktop::base::EventLoop::current()->post([confirm] {});
}

void RestoreDialog::build()
{
    m_dialog = std::make_unique<DialogWindow>(&m_context.window,
                                              format(tr("Restore “{name}”"), {{"name", m_targetName}}), kWidth);
    DialogWindow* dialog = m_dialog.get();
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [dialog] { dialog->close(); });
    m_dialog->onClosed = [this] {
        dropConfirmation();
        if (m_guard)
            m_guard->disarm();
        m_source = nullptr;
        m_problem = nullptr;
        m_restore = nullptr;
    };

    auto* form = m_dialog->setContent<Form>();
    auto* text = form->add<Label>(format(tr("Choose a volume, disk or disk image to copy onto “{name}”. Everything "
                                            "on “{name}” is erased and replaced with the copy."),
                                         {{"name", m_targetName}}));
    text->setWrap(true);

    Card* card = form->addCard();
    auto* row = card->addChild<CardRow>(tr("Restore From"));
    m_source = row->addContent<ChoiceButton>();
    m_source->setName("restore:source");
    m_source->onActivated = [this](int) {
        m_nothingChosen = false;
        m_removed.clear();
        update();
    };
    auto* image = row->addContent<Button>(tr("Image…"));
    image->setName("restore:image");
    image->onClicked = [this] {
        if (m_chooseImage)
            m_chooseImage([this](const std::string& path) { setImage(path); });
    };

    m_problem = form->add<Footnote>();
    m_problem->setName("restore:problem");
    m_problem->setError(true);

    Footer* footer = form->addFooter();
    m_restore = footer->addEnd<DialogButton>(tr("Restore"), true);
    m_restore->setName("restore:restore");
    m_restore->setDestructive(true);
    m_restore->onClicked = [this] { restore(); };
}

// Depth first, as the sidebar lists them with every device shown.
static void collect(const std::vector<udisks::Node>& nodes, std::vector<const udisks::Node*>& out)
{
    for (const udisks::Node& node : nodes) {
        out.push_back(&node);
        collect(node.children, out);
    }
}

void RestoreDialog::reloadSources()
{
    const udisks::Model& model = m_context.model;
    const Source* previous = chosen();
    const images::Endpoint keep = previous ? previous->endpoint : images::Endpoint{};
    const std::string keptName = previous ? previous->name : std::string();
    const bool hadChoice = previous != nullptr;

    m_sources.clear();
    if (!m_image.empty()) {
        const std::string name = std::filesystem::path(m_image).filename().string();
        m_sources.push_back({name, name, images::Endpoint::ofFile(m_image)});
    }
    const std::vector<udisks::SectionNodes> tree = udisks::buildTree(model, udisks::ViewMode::AllDevices);
    std::vector<const udisks::Node*> nodes;
    for (const udisks::SectionNodes& section : tree)
        collect(section.nodes, nodes);
    for (const udisks::Node* node : nodes) {
        if (node->kind == udisks::NodeKind::Container)
            continue;
        const udisks::Object* object = model.object(node->block);
        // What holds the system cannot be unmounted to be read.
        if (!object || !object->block || object->block->size == 0 || model.holdsSystem(node->block)
            || images::overlaps(model, node->block, m_target.block))
            continue;
        const bool listed = std::any_of(m_sources.begin(), m_sources.end(),
                                        [node](const Source& source) { return source.endpoint.block == node->block; });
        if (listed)
            continue;
        const std::string name = udisks::displayName(model, *node);
        const std::string title = format(tr("{name} ({device}, {size})"),
                                         {{"name", name},
                                          {"device", udisks::deviceName(model, node->block)},
                                          {"size", udisks::formatSize(object->block->size)}});
        m_sources.push_back({title, name, images::Endpoint::ofBlock(model, node->block)});
    }

    if (!m_source)
        return;
    // The first source is chosen at first; a chosen one that goes, or whose
    // path another device took, leaves nothing chosen until the user picks
    // again, rather than another disk in its place.
    std::vector<ChoiceButton::Item> titles;
    int current = hadChoice || m_nothingChosen || m_sources.empty() ? -1 : 0;
    for (size_t i = 0; i < m_sources.size(); ++i) {
        titles.push_back({m_sources[i].title, std::to_string(i)});
        if (!hadChoice || m_sources[i].endpoint.block != keep.block || m_sources[i].endpoint.file != keep.file)
            continue;
        if (keep.isBlock() && !model.matches(keep.identity))
            continue;
        current = int(i);
        // The device as it was chosen, for the confirmation's check.
        m_sources[i].endpoint = keep;
    }
    if (hadChoice && current < 0) {
        m_nothingChosen = true;
        m_removed = format(tr("“{name}” is no longer there. Choose another source."), {{"name", keptName}});
    }
    m_source->setItems(titles);
    m_source->setCurrentIndex(current);
    update();
}

void RestoreDialog::setImage(const std::string& path)
{
    m_image = path;
    m_nothingChosen = false;
    m_removed.clear();
    if (!isOpen())
        return;
    reloadSources();
    if (m_source && !m_sources.empty()) {
        m_source->setCurrentIndex(0);
        update();
    }
}

const RestoreDialog::Source* RestoreDialog::chosen() const
{
    if (!m_source)
        return nullptr;
    const int index = m_source->currentIndex();
    return index >= 0 && size_t(index) < m_sources.size() ? &m_sources[size_t(index)] : nullptr;
}

void RestoreDialog::update()
{
    if (!m_restore)
        return;
    const Source* source = chosen();
    std::string problem;
    if (!source)
        problem = tr("Choose a disk image, or attach a disk to restore from.");
    else
        problem = images::restoreProblem(m_context.model, source->endpoint, m_target.block);
    m_problem->setText(source ? problem : m_removed);
    m_restore->setEnabled(source && problem.empty());
}

void RestoreDialog::restore()
{
    const Source* source = chosen();
    if (!source || !images::restoreProblem(m_context.model, source->endpoint, m_target.block).empty())
        return;
    const Source picked = *source;
    const std::string& sourceName = picked.name;
    std::string text = format(tr("Everything on “{target}” is replaced with the contents of “{source}”. This "
                                 "cannot be undone."),
                              {{"target", m_targetName}, {"source", sourceName}});
    if (picked.endpoint.isBlock() && !images::mountedOn(m_context.model, picked.endpoint.block).empty())
        text += " " + format(tr("“{source}” is unmounted before it is read, and stays so afterwards."),
                             {{"source", sourceName}});
    const udisks::Node target = m_target;
    const std::string targetName = m_targetName;
    const udisks::Identity targetIdentity = m_targetIdentity;
    m_confirm = std::make_unique<ConfirmDialog>(m_dialog->window(),
                                                format(tr("Erase “{target}”?"), {{"target", targetName}}), text);
    m_confirm->setConfirmText(tr("Erase and Restore"));
    m_confirm->setDestructive(true);
    m_confirm->onAccepted = [this, picked, sourceName, target, targetName, targetIdentity] {
        // Closing the dialog destroys the question and this callback with it.
        const Source source = picked;
        const std::string from = sourceName;
        const udisks::Node onto = target;
        const std::string ontoName = targetName;
        const udisks::Identity ontoIdentity = targetIdentity;
        // The question is answered; what follows checks the disks itself.
        if (m_guard)
            m_guard->disarm();
        if (isOpen())
            m_dialog->close();
        // The disks may have changed while the question was open.
        if (!m_context.model.matches(ontoIdentity)
            || (source.endpoint.isBlock() && !m_context.model.matches(source.endpoint.identity))) {
            showGone(m_context, m_context.model.matches(ontoIdentity) ? from : ontoName);
            return;
        }
        const std::string problem = images::restoreProblem(m_context.model, source.endpoint, onto.block);
        if (!problem.empty()) {
            m_context.showError(tr("The restore could not start."), problem);
            return;
        }
        if (onRestore)
            onRestore(source.endpoint, from, onto, ontoName);
    };
    m_confirm->show();
}

} // namespace dialogs
