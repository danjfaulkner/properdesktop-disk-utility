// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/volume_actions.h"

#include "dialogs/cards.h"
#include "dialogs/choice_button.h"
#include "dialogs/erase.h"
#include "dialogs/partition.h"
#include "dialogs/plan_runner.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/text_field.h"
#include "ui/window.h"

#include <algorithm>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 400;
static constexpr double kFieldWidth = 180;

AddPartitionDialog::AddPartitionDialog(views::Context& context, std::string disk)
    : m_context(context)
    , m_disk(std::move(disk))
    , m_formats(context.operations)
{
    const udisks::Object* whole = context.model.object(m_disk);
    m_tableType = whole && whole->partitionTable ? whole->partitionTable->type : std::string();
    const uint64_t end = whole && whole->block ? usableEnd(whole->block->size, m_tableType) : 0;
    for (const FreeRegion& region : freeRegions(context.model, m_disk)) {
        const uint64_t stop = std::min(region.offset + region.size, end);
        if (stop > region.offset && stop - region.offset > m_region.size)
            m_region = {region.offset, stop - region.offset};
    }
    size_t primaries = 0;
    for (const udisks::Object* partition : context.model.partitionsOf(m_disk)) {
        if (!partition->partition->isContained)
            ++primaries;
    }
    if (primaries >= maximumPartitions(m_tableType))
        m_refusal = m_tableType == "dos" ? tr("A Master Boot Record holds at most four partitions.")
                                         : tr("The partition map holds no more partitions.");
    else if (m_region.size == 0)
        m_refusal = tr("There is no free space for another partition.");
    m_chosenSize = m_region.size;
    m_identity = udisks::identityOf(context.model, m_disk);
    udisks::Node node;
    node.kind = udisks::NodeKind::Disk;
    node.block = m_disk;
    node.disk = m_disk;
    node.drive = m_identity.drive;
    const std::string diskName = udisks::displayName(context.model, node);

    m_dialog = std::make_unique<DialogWindow>(&context.window, tr("Add Partition"), kWidth);
    m_guard = std::make_unique<IdentityGuard>(context, std::vector<udisks::Identity>{m_identity}, diskName, *m_dialog,
                                              [this] { return m_runner && m_runner->isRunning(); });
    // Nothing of the window is touched once it is gone.
    m_dialog->onClosed = [this] {
        m_guard->disarm();
        m_subscriptions.clear();
        m_name = nullptr;
        m_format = nullptr;
        m_size = nullptr;
        m_note = nullptr;
        m_problem = nullptr;
        m_add = nullptr;
    };
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->onCloseRequested = [this] {
        if (!m_runner || !m_runner->isRunning())
            m_dialog->close();
    };
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] {
        if (!m_runner || !m_runner->isRunning())
            m_dialog->close();
    });

    auto* form = m_dialog->setContent<Form>();
    Card* card = form->addCard();
    m_name = card->addChild<TextFieldRow>(tr("Name"), kFieldWidth)->field();
    m_name->setName("addPartition:name");
    m_name->setText(defaultLabel("ext4"));
    m_name->onTextChanged = [this] {
        // FAT keeps names in capitals; the field shows them so.
        const std::string shown = normalizedLabel(m_format ? m_format->currentValue() : std::string(), m_name->text());
        if (shown != m_name->text()) {
            const size_t cursor = m_name->cursorPosition();
            m_name->setText(shown);
            m_name->setCursorPosition(cursor);
            return;
        }
        update();
    };
    m_name->onAccepted = [this] { addPartition(); };
    m_format = card->addChild<CardRow>(tr("Format"))->addContent<ChoiceButton>();
    m_format->setName("addPartition:format");
    m_format->onActivated = [this](int) {
        bool isDefault = false;
        for (const std::string& type : erasableFilesystems())
            isDefault = isDefault || m_name->text() == defaultLabel(type);
        const std::string type = m_format->currentValue();
        m_name->setText(isDefault ? defaultLabel(type) : normalizedLabel(type, m_name->text()));
        update();
    };
    m_size = card->addChild<TextFieldRow>(tr("Size"), kFieldWidth)->field();
    m_size->setName("addPartition:size");
    m_size->setText(udisks::formatSize(m_chosenSize));
    // Only a typed size is applied: the shown one is rounded.
    m_size->onTextEdited = [this] { m_sizeTyped = true; };
    m_size->onEditingFinished = [this] {
        if (!m_sizeTyped)
            return;
        m_sizeTyped = false;
        if (const std::optional<uint64_t> size = udisks::parseSize(m_size->text(), kGB))
            m_chosenSize = std::clamp(alignDown(*size), std::min(kMiB, m_region.size), m_region.size);
        m_size->setText(udisks::formatSize(m_chosenSize));
        update();
    };
    m_note = form->add<Footnote>(m_refusal.empty()
                                     ? format(tr("Up to {size} of free space, from {offset} into the disk."),
                                              {{"size", udisks::formatSize(m_region.size)},
                                               {"offset", udisks::formatSize(m_region.offset)}})
                                     : m_refusal);
    m_note->setName("addPartition:note");
    m_problem = form->add<Footnote>();
    m_problem->setName("addPartition:problem");
    m_problem->setError(true);
    Footer* footer = form->addFooter();
    m_add = footer->addEnd<DialogButton>(tr("Add"), true);
    m_add->setName("addPartition:add");
    m_add->onClicked = [this] { addPartition(); };

    m_subscriptions.push_back(m_formats.changed.connect([this] { updateFormats(); }));
    updateFormats();
    m_formats.query();
}

AddPartitionDialog::~AddPartitionDialog() = default;

void AddPartitionDialog::show()
{
    m_dialog->show();
    m_name->setFocus();
}

bool AddPartitionDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

void AddPartitionDialog::updateFormats()
{
    if (!m_format)
        return;
    std::vector<ChoiceButton::Item> items;
    for (const std::string& type : erasableFilesystems())
        items.push_back({formatName(type), type, m_formats.isAvailable(type), m_formats.reason(type)});
    const std::string current = m_format->currentValue();
    m_format->setItems(std::move(items));
    m_format->setCurrentValue(current.empty() ? "ext4" : current);
    update();
}

PlanStep AddPartitionDialog::step() const
{
    const std::string type = m_format->currentValue();
    const std::string label = normalizedLabel(type, m_name->text());
    udisks::FormatOptions options;
    options.label = label;
    options.takeOwnership = hasOwners(type);
    return {.kind = PlanStep::Kind::Create, .path = m_disk, .offset = m_region.offset, .size = m_chosenSize,
            .type = type, .partitionName = m_tableType == "gpt" ? partitionNameFor(label) : std::string(),
            .options = options, .subject = label.empty() ? formatName(type) : label};
}

std::string AddPartitionDialog::problem() const
{
    if (!m_refusal.empty())
        return {};
    const std::string type = m_format->currentValue();
    if (m_formats.entry(type).state == FormatAvailability::State::Missing)
        return format(tr("{format} can't be made: {reason}"), {{"format", formatName(type)}, {"reason", m_formats.reason(type)}});
    if (m_chosenSize < minimumSize(type))
        return format(tr("{format} needs at least {size}."),
                      {{"format", formatName(type)}, {"size", udisks::formatSize(minimumSize(type))}});
    return labelProblem(type, m_name->text());
}

void AddPartitionDialog::update()
{
    if (!m_add)
        return;
    const std::string why = problem();
    m_problem->setText(why);
    const bool running = m_runner && m_runner->isRunning();
    m_add->setEnabled(!running && m_refusal.empty() && why.empty() && m_formats.isAvailable(m_format->currentValue()));
}

void AddPartitionDialog::addPartition()
{
    if (!m_add || !m_add->isEnabled() || !m_context.model.matches(m_identity))
        return;
    m_runner = std::make_unique<PlanRunner>(m_context.model, m_context.operations, m_identity,
                                            std::vector<PlanStep>{step()});
    m_subscriptions.push_back(m_runner->stepStarted.connect([this](size_t) {
        m_note->setText(describeRunning(m_runner->steps().front()));
    }));
    m_subscriptions.push_back(m_runner->finished.connect([this] { finished(); }));
    m_name->setEnabled(false);
    m_format->setEnabled(false);
    m_size->setEnabled(false);
    m_runner->start();
    update();
}

void AddPartitionDialog::finished()
{
    const udisks::Result& result = m_runner->result();
    if (m_runner->succeeded()) {
        m_context.toast(format(tr("Added “{name}”."), {{"name", m_runner->steps().front().subject}}));
        m_dialog->close();
        return;
    }
    m_name->setEnabled(true);
    m_format->setEnabled(true);
    m_size->setEnabled(true);
    m_note->setText({});
    if (!result.cancelled())
        m_problem->setText(result.errorMessage.empty() ? std::string(tr("The partition could not be added."))
                                                       : result.errorMessage);
    m_runner.reset();
    update();
}

// The dialogs of the four actions, one of each at a time.
struct VolumeActions
{
    explicit VolumeActions(views::Context& context)
        : context(context)
    {
    }

    views::Context& context;
    std::unique_ptr<EraseDialog> erase;
    std::unique_ptr<PartitionDialog> partition;
    std::unique_ptr<AddPartitionDialog> add;
    std::unique_ptr<ConfirmDialog> confirm;
    std::unique_ptr<IdentityGuard> confirmGuard;
};

static void confirmDelete(VolumeActions& actions, const udisks::Node& node)
{
    views::Context& context = actions.context;
    const std::string name = udisks::displayName(context.model, node);
    std::string text = tr("Everything stored on the partition is deleted. This can't be undone.");
    std::string inside;
    for (const udisks::Object* logical : context.model.logicalPartitionsOf(node.block)) {
        udisks::Node partition = node;
        partition.block = logical->path;
        partition.kind = logical->filesystem || logical->encrypted ? udisks::NodeKind::Volume
                                                                    : udisks::NodeKind::Partition;
        const std::string quoted = format(tr("“{name}”"), {{"name", udisks::displayName(context.model, partition)}});
        inside += (inside.empty() ? "" : ", ") + quoted;
    }
    if (!inside.empty())
        text = format(tr("The partitions in it, {names}, and everything stored on them are deleted. This can't be "
                         "undone."),
                      {{"names", inside}});
    actions.confirm = std::make_unique<ConfirmDialog>(&context.window, format(tr("Delete “{name}”?"), {{"name", name}}),
                                                      text);
    actions.confirm->setConfirmText(tr("Delete"));
    actions.confirm->setDestructive(true);
    const udisks::Identity identity = udisks::identityOf(context.model, node.block);
    VolumeActions* owner = &actions;
    actions.confirmGuard = std::make_unique<IdentityGuard>(context, std::vector<udisks::Identity>{identity}, name,
                                                           [owner] { owner->confirm.reset(); });
    // The answer ends what the guard watches for.
    actions.confirm->onRejected = [owner] { owner->confirmGuard->disarm(); };
    actions.confirm->onAccepted = [owner, &context, identity, name] {
        owner->confirmGuard->disarm();
        if (!context.model.matches(identity)) {
            showGone(context, name);
            return;
        }
        context.operations.deletePartition(identity.block, [&context, name](const udisks::Result& result) {
            if (result.ok())
                context.toast(format(tr("Deleted “{name}”."), {{"name", name}}));
            else if (!result.cancelled() && result.errorName != udisks::Operations::kSystemError)
                context.showError(format(tr("“{name}” could not be deleted."), {{"name", name}}), result.errorMessage);
        });
    };
    actions.confirm->show();
}

void installVolumeActions(views::Context& context)
{
    auto actions = std::make_shared<VolumeActions>(context);
    context.actions.setHandler(Action::Erase, [actions](const Selection& selection) {
        if (!selection)
            return;
        actions->erase = std::make_unique<EraseDialog>(actions->context, *selection);
        actions->erase->show();
    });
    context.actions.setHandler(Action::Partition, [actions](const Selection& selection) {
        if (!selection)
            return;
        actions->partition = std::make_unique<PartitionDialog>(actions->context, selection->disk);
        actions->partition->show();
    });
    context.actions.setHandler(Action::AddVolume, [actions](const Selection& selection) {
        if (!selection)
            return;
        actions->add = std::make_unique<AddPartitionDialog>(actions->context, selection->disk);
        actions->add->show();
    });
    context.actions.setHandler(Action::RemoveVolume, [actions](const Selection& selection) {
        if (selection)
            confirmDelete(*actions, *selection);
    });
}

} // namespace dialogs
