// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/partition.h"

#include "dialogs/cards.h"
#include "dialogs/choice_button.h"
#include "dialogs/pie_chart.h"
#include "dialogs/plan_runner.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/header_button.h"
#include "ui/header_button_group.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/progress_bar.h"
#include "ui/text_field.h"
#include "ui/window.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 640;
static constexpr double kFieldWidth = 180;
static constexpr double kColumnGap = 24;
static constexpr double kButtonsGap = 8;

// The pie with + and - centred under it, and the slice's fields beside it.
class PartitionEditor : public View
{
public:
    PartitionEditor()
    {
        pie = addChild<PieChart>();
        pie->setName("partition:pie");
        buttons = addChild<HeaderButtonGroup>();
        add = buttons->addButton("add");
        add->setName("partition:add");
        add->setToolTip(tr("Add Partition"));
        remove = buttons->addButton("remove");
        remove->setName("partition:remove");
        remove->setToolTip(tr("Delete Partition"));
        fields = addChild<Form>();
    }

    double heightForWidth(double width) const override
    {
        const double left = PieChart::kSize + kButtonsGap + buttons->sizeHint().height;
        const double right = fields->heightForWidth(std::max(0.0, width - PieChart::kSize - kColumnGap));
        return std::ceil(std::max(left, right));
    }
    SizeF sizeHint() const override { return {0, heightForWidth(geometry().width)}; }

    PieChart* pie;
    HeaderButtonGroup* buttons;
    HeaderButton* add;
    HeaderButton* remove;
    Form* fields;

protected:
    void layout() override
    {
        const double width = geometry().width;
        pie->setGeometry({0, 0, PieChart::kSize, PieChart::kSize});
        const SizeF group = buttons->sizeHint();
        buttons->setGeometry({std::round((PieChart::kSize - group.width) / 2), PieChart::kSize + kButtonsGap,
                              group.width, group.height});
        const double x = PieChart::kSize + kColumnGap;
        const double right = std::max(0.0, width - x);
        fields->setGeometry({x, 0, right, std::ceil(fields->heightForWidth(right))});
    }
};

PartitionDialog::PartitionDialog(views::Context& context, std::string disk)
    : m_context(context)
    , m_disk(std::move(disk))
    , m_formats(context.operations)
{
    m_original = PartitionLayout::fromDisk(context.model, m_disk);
    m_layout = m_original;
    const udisks::Object* whole = context.model.object(m_disk);
    udisks::Node node;
    node.kind = udisks::NodeKind::Disk;
    node.block = m_disk;
    node.disk = m_disk;
    node.drive = whole && whole->block ? whole->block->drive : std::string();
    m_diskName = udisks::displayName(context.model, node);
    m_identity = udisks::identityOf(context.model, m_disk);

    m_dialog = std::make_unique<DialogWindow>(&context.window, format(tr("Partition “{name}”"), {{"name", m_diskName}}),
                                              kWidth);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->onCloseRequested = [this] {
        if (!isRunning())
            m_dialog->close();
    };
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] {
        if (!isRunning())
            m_dialog->close();
    });
    m_guard = std::make_unique<IdentityGuard>(context, std::vector<udisks::Identity>{m_identity}, m_diskName,
                                              *m_dialog, [this] { return isRunning(); });
    // Nothing of the window is touched once it is gone.
    m_dialog->onClosed = [this] {
        m_guard->disarm();
        m_subscriptions.clear();
        forgetViews();
    };
    m_subscriptions.push_back(m_formats.changed.connect([this] { refresh(); }));
    m_subscriptions.push_back(context.model.changed.connect([this] {
        if (!m_context.model.object(m_disk) && !isRunning() && isOpen())
            m_dialog->close();
        updateProgress();
    }));

    for (const std::string& type : m_layout.resizableFilesystems()) {
        context.operations.canResize(type, [this, type, alive = std::weak_ptr<bool>(m_alive)](
                                               bool available, uint64_t modes, const std::string& tool) {
            if (alive.expired())
                return;
            m_original.setResizeSupport(type, available, modes, tool);
            m_layout.setResizeSupport(type, available, modes, tool);
            refresh();
        });
    }
    for (const Slice& slice : m_layout.slices()) {
        if (slice.kind == Slice::Kind::Partition) {
            m_selected = slice.id;
            break;
        }
    }
    if (!m_selected && !m_layout.slices().empty())
        m_selected = m_layout.slices().front().id;
    buildEditor();
    m_formats.query();
}

PartitionDialog::~PartitionDialog() = default;

void PartitionDialog::show()
{
    m_dialog->show();
}

bool PartitionDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

bool PartitionDialog::isRunning() const
{
    return m_runner && m_runner->isRunning();
}

void PartitionDialog::buildEditor()
{
    auto* form = m_dialog->setContent<Form>();
    const udisks::Object* whole = m_context.model.object(m_disk);
    const uint64_t size = whole && whole->block ? whole->block->size : 0;
    auto* intro = form->add<Label>(
        m_layout.createsTable()
            ? format(tr("“{name}” ({size}) has no partition map. Click + to add partitions; Apply creates a GUID "
                        "Partition Map for them, erasing what is on the disk."),
                     {{"name", m_diskName}, {"size", udisks::formatSize(size)}})
            : format(tr("{scheme}, {size}. Click + to add a partition, drag the handles on the rim to resize. "
                        "Nothing changes until you click Apply."),
                     {{"scheme", udisks::schemeName(m_layout.tableType())}, {"size", udisks::formatSize(size)}}));
    intro->setWrap(true);
    intro->setName("partition:intro");

    auto* editor = form->add<PartitionEditor>();
    m_pie = editor->pie;
    m_add = editor->add;
    m_remove = editor->remove;
    m_pie->setRange(m_layout.start(), m_layout.end());
    // The pie takes no focus: a size typed before the press is applied
    // first, as it can split the wedges and move the handles' indices.
    m_pie->onPressed = [this] { sizeEdited(); };
    m_pie->onSelected = [this](uint32_t id) { select(id); };
    m_pie->onBoundaryMoved = [this](size_t index, uint64_t position) { moveBoundary(index, position, false); };
    m_pie->onDragFinished = [this] {
        m_layout.normalize();
        refresh();
    };
    m_add->onClicked = [this] { add(); };
    m_remove->onClicked = [this] { remove(); };

    Card* card = editor->fields->addCard();
    m_name = card->addChild<TextFieldRow>(tr("Name"), kFieldWidth)->field();
    m_name->setName("partition:name");
    m_name->onTextChanged = [this] {
        const Slice* slice = m_selected ? m_layout.slice(*m_selected) : nullptr;
        if (!slice)
            return;
        // FAT keeps names in capitals; the field shows them so.
        const std::string shown = normalizedLabel(slice->filesystem, m_name->text());
        if (shown != m_name->text()) {
            const size_t cursor = m_name->cursorPosition();
            m_name->setText(shown);
            m_name->setCursorPosition(cursor);
            return;
        }
        m_layout.setName(*m_selected, shown);
        refresh();
    };
    m_format = card->addChild<CardRow>(tr("Format"))->addContent<ChoiceButton>();
    m_format->setName("partition:format");
    m_format->onActivated = [this](int) {
        if (!m_selected)
            return;
        const Slice* slice = m_layout.slice(*m_selected);
        const std::string type = m_format->currentValue();
        // A name still at its default follows the format's capitals.
        if (slice && slice->kind == Slice::Kind::New && slice->name == defaultLabel(slice->filesystem))
            m_layout.setName(*m_selected, defaultLabel(type));
        else if (slice)
            m_layout.setName(*m_selected, normalizedLabel(type, slice->name));
        m_layout.setFilesystem(*m_selected, type);
        refresh();
    };
    m_size = card->addChild<TextFieldRow>(tr("Size"), kFieldWidth)->field();
    m_size->setName("partition:size");
    // A typed size belongs to the slice it was typed for, and only a typed
    // one is applied: the shown text is rounded.
    m_size->onTextEdited = [this] { m_sizeSlice = m_selected; };
    m_size->onEditingFinished = [this] { sizeEdited(); };
    m_note = editor->fields->add<Footnote>();
    m_note->setName("partition:note");

    m_problem = form->add<Footnote>();
    m_problem->setName("partition:problem");
    m_problem->setError(true);

    Footer* footer = form->addFooter();
    m_revert = footer->addStart<DialogButton>(tr("Revert"));
    m_revert->setName("partition:revert");
    m_revert->onClicked = [this] { revert(); };
    m_apply = footer->addEnd<DialogButton>(tr("Apply"), true);
    m_apply->setName("partition:apply");
    m_apply->onClicked = [this] { showSummary(); };
    refresh();
}

void PartitionDialog::forgetViews()
{
    m_pie = nullptr;
    m_add = nullptr;
    m_remove = nullptr;
    m_name = nullptr;
    m_format = nullptr;
    m_size = nullptr;
    m_note = nullptr;
    m_problem = nullptr;
    m_apply = nullptr;
    m_revert = nullptr;
    m_status = nullptr;
    m_bar = nullptr;
    m_stepNote = nullptr;
    m_error = nullptr;
    m_done = nullptr;
    m_stop = nullptr;
}

void PartitionDialog::select(uint32_t id)
{
    if (!m_layout.slice(id))
        return;
    if (m_sizeSlice && m_sizeSlice != id)
        sizeEdited();
    m_selected = id;
    refresh();
}

void PartitionDialog::moveBoundary(size_t index, uint64_t position, bool finished)
{
    // The pie takes no focus: a size typed before the drag goes first.
    sizeEdited();
    m_layout.moveBoundary(index, position);
    if (finished)
        m_layout.normalize();
    refresh();
}

void PartitionDialog::refresh()
{
    if (!m_pie)
        return;
    // A slice merged into its neighbour hands the selection to what now
    // covers its place.
    if (m_selected && !m_layout.slice(*m_selected))
        m_selected.reset();
    if (!m_selected && !m_layout.slices().empty())
        m_selected = m_layout.slices().front().id;

    std::vector<PieChart::Wedge> wedges;
    std::vector<size_t> movable;
    const auto& slices = m_layout.slices();
    for (size_t i = 0; i < slices.size(); ++i) {
        const Slice& slice = slices[i];
        PieChart::Wedge wedge{slice.id, slice.offset, slice.size};
        wedge.free = slice.kind == Slice::Kind::Free;
        wedge.name = wedge.free ? tr("Free Space")
                : slice.kind == Slice::Kind::Partition && slice.name.empty() ? slice.displayName
                                                                             : slice.name;
        if (slice.kind == Slice::Kind::Container)
            wedge.name = slice.displayName;
        wedge.sizeText = udisks::formatSize(slice.size);
        wedges.push_back(std::move(wedge));
        if (i + 1 < slices.size() && m_layout.canMoveBoundary(i))
            movable.push_back(i);
    }
    m_pie->setWedges(std::move(wedges));
    m_pie->setMovableBoundaries(std::move(movable));
    m_pie->setSelected(m_selected);
    refreshFields();
    refreshButtons();
}

void PartitionDialog::refreshFields()
{
    const Slice* slice = m_selected ? m_layout.slice(*m_selected) : nullptr;
    const bool editable = slice && slice->isEditable();

    if (m_name->text() != (slice ? slice->name : std::string()) || !editable)
        m_name->setText(editable ? slice->name : std::string());
    // A name goes to a new filesystem, or one UDisks2 can rename.
    m_name->setEnabled(editable && (slice->kind == Slice::Kind::New || slice->reformats() || slice->hasFilesystem));
    m_name->setPlaceholder(slice && slice->kind == Slice::Kind::Free ? tr("Free Space") : std::string());

    std::vector<ChoiceButton::Item> items;
    if (slice && slice->kind == Slice::Kind::Partition) {
        const auto& known = erasableFilesystems();
        if (std::find(known.begin(), known.end(), slice->originalFilesystem) == known.end()) {
            const std::string name = slice->originalFilesystem.empty()
                    ? std::string(tr("Unformatted"))
                    : udisks::filesystemName(slice->originalFilesystem);
            items.push_back({name, slice->originalFilesystem});
        }
    }
    if (editable) {
        for (const std::string& type : erasableFilesystems()) {
            ChoiceButton::Item item{formatName(type), type};
            const bool current = slice->kind == Slice::Kind::Partition && type == slice->originalFilesystem;
            item.enabled = current || m_formats.isAvailable(type);
            item.reason = item.enabled ? std::string() : m_formats.reason(type);
            items.push_back(std::move(item));
        }
    } else {
        items.push_back({slice && slice->kind == Slice::Kind::Container ? slice->displayName : tr("Free Space"), {}});
    }
    m_format->setItems(std::move(items));
    if (editable)
        m_format->setCurrentValue(slice->filesystem);
    m_format->setEnabled(editable);

    if (!m_sizeSlice || m_sizeSlice != m_selected)
        m_size->setText(slice ? udisks::formatSize(slice->size) : std::string());
    std::string note;
    bool resizable = false;
    if (slice && editable) {
        const auto [low, high] = m_layout.sizeRange(slice->id);
        resizable = high >= low + kMiB;
        if (resizable)
            note = format(tr("Between {minimum} and {maximum}."),
                          {{"minimum", udisks::formatSize(low)}, {"maximum", udisks::formatSize(high)}});
        if (slice->kind == Slice::Kind::Partition) {
            const ActionState shrink = m_layout.shrinkState(slice->id);
            const ActionState grow = m_layout.growState(slice->id);
            const std::string& why = !shrink.enabled ? shrink.reason : grow.reason;
            if (!why.empty())
                note = note.empty() ? why : note + " " + why;
            if (shrink.enabled && !slice->reformats() && slice->used == 0)
                note += " " + std::string(tr("How far the filesystem can shrink is only known when it is applied; "
                                             "a shrink runs before anything is deleted."));
            if (slice->reformats())
                note = format(tr("Changing the format erases everything on “{name}”."), {{"name", slice->displayName}})
                        + (note.empty() ? std::string() : " " + note);
        }
    } else if (slice && slice->kind == Slice::Kind::Free) {
        note = tr("Click + to add a partition in this free space.");
    } else if (slice && slice->kind == Slice::Kind::Container) {
        note = m_layout.removeState(slice->id).reason;
    }
    m_size->setEnabled(resizable);
    m_note->setText(note);
}

void PartitionDialog::refreshButtons()
{
    m_add->setEnabled(m_layout.addState(m_selected).enabled);
    m_add->setToolTip(m_layout.addState(m_selected).enabled ? std::string(tr("Add Partition"))
                                                            : m_layout.addState(m_selected).reason);
    const ActionState removable = m_selected ? m_layout.removeState(*m_selected) : ActionState{};
    m_remove->setEnabled(removable.enabled);
    m_remove->setToolTip(removable.enabled ? std::string(tr("Delete Partition")) : removable.reason);

    std::string why = m_layout.problem();
    if (why.empty()) {
        for (const Slice& slice : m_layout.slices()) {
            const bool formats = slice.kind == Slice::Kind::New || slice.reformats();
            if (formats && !m_formats.isAvailable(slice.filesystem)) {
                why = format(tr("{format} can't be made: {reason}"),
                             {{"format", formatName(slice.filesystem)}, {"reason", m_formats.reason(slice.filesystem)}});
                break;
            }
        }
    }
    m_problem->setText(why);
    m_apply->setEnabled(why.empty() && m_layout.isChanged());
    m_revert->setEnabled(m_layout.isChanged());
}

std::string PartitionDialog::newFilesystem() const
{
    if (m_formats.isAvailable("ext4"))
        return "ext4";
    for (const std::string& type : erasableFilesystems()) {
        if (m_formats.isAvailable(type))
            return type;
    }
    return "ext4";
}

void PartitionDialog::add()
{
    // + and - take no focus: a size typed before them goes to its slice.
    sizeEdited();
    const std::string type = newFilesystem();
    if (const std::optional<uint32_t> id = m_layout.add(m_selected, type, defaultLabel(type)))
        m_selected = id;
    refresh();
}

void PartitionDialog::remove()
{
    sizeEdited();
    if (!m_selected)
        return;
    const Slice* slice = m_layout.slice(*m_selected);
    const uint64_t offset = slice ? slice->offset : 0;
    if (!m_layout.remove(*m_selected))
        return;
    m_selected.reset();
    for (const Slice& other : m_layout.slices()) {
        if (offset >= other.offset && offset < other.end())
            m_selected = other.id;
    }
    refresh();
}

void PartitionDialog::revert()
{
    m_sizeSlice.reset();
    m_layout = m_original;
    m_selected.reset();
    for (const Slice& slice : m_layout.slices()) {
        if (slice.kind == Slice::Kind::Partition && !m_selected)
            m_selected = slice.id;
    }
    refresh();
}

void PartitionDialog::sizeEdited()
{
    if (!m_sizeSlice || !m_size)
        return;
    const uint32_t id = *m_sizeSlice;
    m_sizeSlice.reset();
    if (!m_layout.slice(id))
        return;
    if (const std::optional<uint64_t> size = udisks::parseSize(m_size->text(), kGB)) {
        const auto [low, high] = m_layout.sizeRange(id);
        // The shown sizes are rounded: a typed size at a limit means the limit.
        m_layout.setSize(id, std::clamp(*size, low, high));
    }
    refresh();
}

void PartitionDialog::showSummary()
{
    sizeEdited();
    if (!m_apply || !m_apply->isEnabled())
        return;
    m_layout.updateTeardown(m_context.model);
    const std::vector<PlanStep> steps = m_layout.plan();
    forgetViews();
    auto* form = m_dialog->setContent<Form>();
    auto* message = form->add<Label>(format(tr("These steps will be taken on “{name}”, in this order:"),
                                            {{"name", m_diskName}}));
    message->setWrap(true);
    message->setName("partition:summaryIntro");
    Card* card = form->addCard();
    for (size_t i = 0; i < steps.size(); ++i) {
        auto* row = card->addChild<CardRow>(describe(steps[i]));
        row->setName("partition:summary:" + std::to_string(i));
    }
    const bool destroys = std::any_of(steps.begin(), steps.end(), [](const PlanStep& step) {
        return step.kind == PlanStep::Kind::Delete || step.kind == PlanStep::Kind::Format
                || step.kind == PlanStep::Kind::CreateTable;
    });
    form->add<Footnote>(destroys ? tr("Deleted and erased volumes lose everything stored on them. Resizing can take "
                                      "a while; keep the disk connected until it is done.")
                                 : tr("Resizing can take a while; keep the disk connected until it is done."));
    Footer* footer = form->addFooter();
    auto* back = footer->addStart<DialogButton>(tr("Back"));
    back->setName("partition:back");
    back->onClicked = [this] { buildEditor(); };
    auto* partition = footer->addEnd<DialogButton>(tr("Partition"), true);
    partition->setName("partition:confirm");
    partition->setDestructive(destroys);
    partition->onClicked = [this] { apply(); };
}

void PartitionDialog::apply()
{
    if (isRunning() || !m_context.model.matches(m_identity))
        return;
    m_layout.updateTeardown(m_context.model);
    std::vector<PlanStep> steps = m_layout.plan();
    if (steps.empty())
        return;
    m_stopped = false;
    m_runner = std::make_unique<PlanRunner>(m_context.model, m_context.operations, m_identity, std::move(steps));
    m_subscriptions.push_back(m_runner->stepStarted.connect([this](size_t index) { stepStarted(index); }));
    m_subscriptions.push_back(m_runner->finished.connect([this] { finished(); }));

    forgetViews();
    auto* form = m_dialog->setContent<Form>();
    m_status = form->add<Label>();
    m_status->setWrap(true);
    m_status->setName("partition:status");
    m_bar = form->add<ProgressBar>();
    m_bar->setName("partition:progress");
    m_bar->setLayoutHints({.fillWidth = true});
    m_bar->setIndeterminate(true);
    m_stepNote = form->add<Footnote>();
    m_stepNote->setName("partition:step");
    m_error = form->add<Footnote>();
    m_error->setName("partition:error");
    m_error->setError(true);
    Footer* footer = form->addFooter();
    m_stop = footer->addStart<DialogButton>(tr("Stop"));
    m_stop->setName("partition:stop");
    m_stop->setVisible(false);
    m_stop->onClicked = [this] { stop(); };
    m_done = footer->addEnd<DialogButton>(tr("Done"), true);
    m_done->setName("partition:done");
    m_done->setVisible(false);
    m_done->onClicked = [this] { m_dialog->close(); };
    m_runner->start();
}

void PartitionDialog::stepStarted(size_t index)
{
    if (!m_status || !m_runner)
        return;
    const auto& steps = m_runner->steps();
    m_status->setText(describeRunning(steps[index]));
    m_stepNote->setText(format(tr("Step {step} of {count}"),
                               {{"step", std::to_string(index + 1)}, {"count", std::to_string(steps.size())}}));
    m_bar->setIndeterminate(true);
    updateProgress();
}

void PartitionDialog::updateProgress()
{
    if (!isRunning() || !m_bar)
        return;
    const PlanStep& step = m_runner->steps()[m_runner->current()];
    const udisks::Object* job = jobOf(m_context.model, {step.path, m_disk});
    m_stop->setVisible(job && job->job->cancelable && !m_stopped);
    if (job && job->job->progressValid) {
        m_bar->setIndeterminate(false);
        m_bar->setValue(job->job->progress);
    }
}

// UDisks2 cancels the job where its tool allows; the step then fails as
// cancelled and the plan stops there.
void PartitionDialog::stop()
{
    if (!isRunning())
        return;
    const PlanStep& step = m_runner->steps()[m_runner->current()];
    const udisks::Object* job = jobOf(m_context.model, {step.path, m_disk});
    if (!job || !job->job->cancelable)
        return;
    m_stopped = true;
    m_stop->setVisible(false);
    m_status->setText(format(tr("Stopping “{step}”…"), {{"step", describe(step)}}));
    m_context.operations.cancelJob(job->path);
}

void PartitionDialog::finished()
{
    if (!m_runner || !m_status)
        return;
    const udisks::Result& result = m_runner->result();
    m_bar->setIndeterminate(false);
    m_stop->setVisible(false);
    m_done->setVisible(true);
    m_done->setFocus();
    m_stepNote->setText({});
    if (m_runner->succeeded()) {
        m_bar->setValue(1);
        m_status->setText(format(tr("“{name}” was partitioned."), {{"name", m_diskName}}));
        return;
    }
    m_bar->setVisible(false);
    const size_t failed = m_runner->current();
    const bool changed = failed > 0 || (erasesData(m_runner->steps()[failed]) && ranPartWay(result));
    if (!changed)
        m_status->setText(format(tr("Nothing was changed on “{name}”."), {{"name", m_diskName}}));
    else if (failed == 0)
        m_status->setText(format(tr("“{name}” was partitioned only in part: “{step}” may have changed it before it "
                                    "failed."),
                                 {{"name", m_diskName}, {"step", describe(m_runner->steps()[0])}}));
    else
        m_status->setText(format(tr("“{name}” was partitioned only in part: the first {count} steps were taken."),
                                 {{"name", m_diskName}, {"count", std::to_string(failed)}}));
    const std::string step = describe(m_runner->steps()[m_runner->failedStep()]);
    if (m_stopped && result.cancelled())
        m_error->setText(format(tr("“{step}” was stopped before it finished."), {{"step", step}}));
    else if (result.cancelled())
        m_error->setText(format(tr("“{step}” was cancelled."), {{"step", step}}));
    else
        m_error->setText(result.errorMessage.empty()
                                 ? format(tr("“{step}” failed."), {{"step", step}})
                                 : format(tr("“{step}” failed: {message}"),
                                          {{"step", step}, {"message", result.errorMessage}}));
}

} // namespace dialogs
