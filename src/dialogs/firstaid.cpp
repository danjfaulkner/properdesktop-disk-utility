// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/firstaid.h"

#include "dialogs/cards.h"
#include "dialogs/mount.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/errors.h"
#include "udisks/holders.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/event.h"
#include "ui/icon.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/progress_bar.h"
#include "ui/text_area.h"
#include "ui/theme.h"
#include "ui/window.h"

#include <algorithm>
#include <cmath>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using udisks::Object;

namespace dialogs {

static constexpr double kWidth = 440;
static constexpr double kLogHeight = 180;

// "Show Details": a chevron and its text, turning down while the details
// show.
class Disclosure : public View
{
public:
    Disclosure()
    {
        m_icon = addChild<Icon>("chevron-right", 12);
        label = addChild<Label>();
        label->setColor(ColorId::DisabledText);
    }

    void setOpen(bool open)
    {
        m_icon->setIconName(open ? "chevron-down" : "chevron-right");
        label->setText(open ? tr("Hide Details") : tr("Show Details"));
        requestLayout();
    }

    Label* label;
    std::function<void()> onClicked;

    SizeF sizeHint() const override
    {
        const SizeF text = label->sizeHint();
        return {12 + 4 + std::ceil(text.width), std::max(12.0, std::ceil(text.height))};
    }
    double heightForWidth(double) const override { return sizeHint().height; }
    bool acceptsPointer() const override { return true; }

protected:
    void layout() override
    {
        const double height = geometry().height;
        m_icon->setGeometry({0, std::round((height - 12) / 2), 12, 12});
        const SizeF text = label->sizeHint();
        label->setGeometry({16, std::round((height - text.height) / 2), std::ceil(text.width), text.height});
    }
    void pointerRelease(const PointerEvent& event) override
    {
        if (event.button == MouseButton::Left && localRect().contains(event.position) && onClicked)
            onClicked();
    }

private:
    Icon* m_icon;
};

// The log in a card, its own height, scrolling inside.
class LogCard : public Card
{
public:
    LogCard()
    {
        setPadding({metrics::cardPadding, 12, metrics::cardPadding, 12});
        log = addChild<TextArea>();
        log->setName("firstAid:log");
        log->setReadOnly(true);
        log->setFrameVisible(false);
        log->setPadding({0, 0, 0, 0});
        log->setWrapMode(TextArea::WrapMode::Wrap);
        log->setFontSize(Theme::current().smallFontSize());
        log->setLayoutHints({.fillWidth = true, .preferredHeight = kLogHeight - 24});
    }

    TextArea* log;
};

FirstAid::FirstAid(views::Context& context)
    : m_context(context)
{
    context.actions.setHandler(Action::FirstAid, [this](const Selection& selection) { start(selection); });
    context.actions.setRefinement(Action::FirstAid, [this](const Selection& selection) { return refine(selection); });
}

FirstAid::~FirstAid()
{
    m_context.actions.setRefinement(Action::FirstAid, {});
    m_context.actions.setHandler(Action::FirstAid, {});
}

ActionState FirstAid::refine(const Selection& selection) const
{
    if (!selection || selection->kind != udisks::NodeKind::Volume)
        return {true, {}};
    const Object* content = m_context.model.contentOf(selection->block);
    if (!content || !content->block)
        return {true, {}};
    const auto missing = m_missingTools.find(content->block->idType);
    if (missing == m_missingTools.end())
        return {true, {}};
    return {false, format(tr("Checking {format} volumes needs {tool}, which is not installed."),
                          {{"format", udisks::filesystemName(content->block->idType)}, {"tool", missing->second}})};
}

static bool isMounted(const Object* block)
{
    return block && block->filesystem && !block->filesystem->mountPoints.empty();
}

std::vector<FirstAid::Volume> FirstAid::volumesOf(const udisks::Node& node) const
{
    const udisks::Model& model = m_context.model;
    std::vector<const Object*> blocks;
    if (node.kind == udisks::NodeKind::Disk)
        blocks = volumesOnDisk(model, node.disk);
    else
        blocks.push_back(model.object(node.block));
    std::vector<Volume> volumes;
    for (const Object* block : blocks) {
        if (!block || !block->block)
            continue;
        Volume volume;
        // A cleartext block is listed by its encrypted one.
        const std::string& backing = block->block->cryptoBackingDevice;
        volume.entry = backing.empty() ? block->path : backing;
        volume.name = volumeName(m_context, volume.entry);
        volume.identity = udisks::identityOf(model, volume.entry);
        // An unlocked volume holding no filesystem has nothing to check.
        if (resolve(volume) == Found::NoFilesystem)
            continue;
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

FirstAid::Found FirstAid::resolve(Volume& volume) const
{
    const udisks::Model& model = m_context.model;
    const Object* entry = model.object(volume.entry);
    if (!entry || !entry->block || (volume.identity.isValid() && !model.matches(volume.identity)))
        return Found::Gone;
    const Object* content = entry;
    if (entry->encrypted) {
        content = model.cleartextOf(entry->path);
        if (!content || !content->block)
            return Found::Locked;
    }
    if (!content->filesystem)
        return entry->encrypted ? Found::NoFilesystem : Found::Gone;
    volume.block = content->path;
    volume.type = content->block->idType;
    volume.wasMounted = isMounted(content);
    return Found::Ready;
}

void FirstAid::release()
{
    m_hold.reset();
}

void FirstAid::rehold()
{
    if (m_running && !m_disk.empty())
        m_hold = m_context.operations.hold(m_disk);
}

void FirstAid::heldThrough()
{
    // A call refused at once answers on the next turn of the loop, and has
    // not made the disk busy meanwhile.
    if (m_running && !m_context.operations.isBusy(m_disk))
        rehold();
}

bool FirstAid::stillThere()
{
    Volume& volume = m_volumes[m_index];
    // Unmounted by us now: what counts is whether it was before.
    const bool wasMounted = volume.wasMounted;
    const bool found = resolve(volume) == Found::Ready;
    volume.wasMounted = wasMounted;
    if (!found)
        log(format(tr("“{name}” is no longer there."), {{"name", volume.name}}));
    return found;
}

void FirstAid::start(const Selection& selection)
{
    if (!selection)
        return;
    if (m_running && m_dialog && m_dialog->window()) {
        m_dialog->window()->activate();
        return;
    }
    if (m_running)
        return;
    const udisks::Node node = *selection;
    const std::string name = udisks::displayName(m_context.model, node);
    std::vector<Volume> volumes = volumesOf(node);
    const bool anyMounted = std::any_of(volumes.begin(), volumes.end(), [](const Volume& v) { return v.wasMounted; });

    std::string text;
    if (node.kind == udisks::NodeKind::Disk)
        text = anyMounted ? tr("First Aid checks every volume on the disk for errors and repairs the ones it finds. "
                               "Mounted volumes are unmounted while they are checked and mounted again afterwards.")
                          : tr("First Aid checks every volume on the disk for errors and repairs the ones it finds.");
    else
        text = anyMounted ? tr("First Aid checks the volume for errors and repairs the ones it finds. The volume is "
                               "unmounted while it is checked, so its files can’t be used until it’s done.")
                          : tr("First Aid checks the volume for errors and repairs the ones it finds.");
    m_confirm = std::make_unique<ConfirmDialog>(&m_context.window,
                                                format(tr("Run First Aid on “{name}”?"), {{"name", name}}), text);
    m_confirm->setConfirmText(tr("Run"));
    // The disk may change while the question is open: the volumes are
    // listed when it is answered, and each found again at its turn. Another
    // disk at its path closes the question.
    const udisks::Identity identity = udisks::identityOf(m_context.model, node.block);
    m_confirmGuard = std::make_unique<IdentityGuard>(m_context, std::vector<udisks::Identity>{identity}, name,
                                                     [this] { m_confirm.reset(); });
    // Answered, the question needs no guard: one left would raise its alert
    // when the disk is ejected later.
    m_confirm->onRejected = [this] { m_confirmGuard->disarm(); };
    m_confirm->onAccepted = [this, node, identity, name, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired())
            return;
        m_confirmGuard->disarm();
        if (m_running)
            return;
        if (!m_context.model.matches(identity)) {
            showGone(m_context, name);
            return;
        }
        run(node);
    };
    m_confirm->show();
}

void FirstAid::run(const udisks::Node& node)
{
    m_volumes = volumesOf(node);
    m_index = 0;
    m_running = true;
    m_cancelled = false;
    m_failed = 0;
    m_skipped = 0;
    m_unrepaired = 0;
    m_log.clear();
    m_disk = node.disk;
    rehold();
    showDialog();
    if (m_volumes.empty()) {
        log(tr("There are no volumes to check."));
        finish();
        return;
    }
    checkVolume();
}

void FirstAid::nextVolume()
{
    ++m_index;
    if (m_cancelled || m_index >= m_volumes.size()) {
        finish();
        return;
    }
    checkVolume();
}

void FirstAid::checkVolume()
{
    Volume& volume = m_volumes[m_index];
    m_problem = false;
    setStatus(format(tr("Checking “{name}”…"), {{"name", volume.name}}));
    std::string skipped;
    switch (resolve(volume)) {
    case Found::Ready:
        break;
    case Found::Gone:
        skipped = tr("“{name}” is no longer there. Skipped.");
        break;
    case Found::Locked:
        skipped = tr("“{name}” is locked. Unlock it to check it. Skipped.");
        break;
    case Found::NoFilesystem:
        skipped = tr("“{name}” holds no file system First Aid can check. Skipped.");
        break;
    }
    if (!skipped.empty()) {
        log(format(skipped, {{"name", volume.name}}));
        ++m_skipped;
        nextVolume();
        return;
    }
    const std::string device = udisks::deviceName(m_context.model, volume.block);
    const std::string type = udisks::filesystemName(volume.type);
    log(format(tr("Checking “{name}” ({device}, {format})."),
               {{"name", volume.name}, {"device", device}, {"format", type.empty() ? tr("unknown format") : type}}));
    m_context.operations.canCheck(volume.type, [this, alive = std::weak_ptr<bool>(m_alive)](
                                                       bool available, const std::string& missing) {
        if (alive.expired())
            return;
        Volume& volume = m_volumes[m_index];
        // UDisks2 names the missing tool; no name is a failed or timed-out
        // question, which says nothing about the tools.
        if (!available && missing.empty()) {
            log(format(tr("UDisks2 could not tell whether {format} volumes can be checked. Try again later."),
                       {{"format", udisks::filesystemName(volume.type)}}));
            ++m_failed;
            nextVolume();
            return;
        }
        if (!available) {
            log(format(tr("{format} volumes can’t be checked: {tool} is not installed."),
                       {{"format", udisks::filesystemName(volume.type)}, {"tool", missing}}));
            m_missingTools[volume.type] = missing;
            m_context.actions.refresh();
            ++m_failed;
            nextVolume();
            return;
        }
        if (!volume.wasMounted) {
            runCheck();
            return;
        }
        log(format(tr("Unmounting “{name}”."), {{"name", volume.name}}));
        const std::vector<std::string> mountPoints = [&] {
            const Object* object = m_context.model.object(volume.block);
            return object && object->filesystem ? object->filesystem->mountPoints : std::vector<std::string>();
        }();
        release();
        m_context.operations.unmount(volume.block, [this, mountPoints, alive](const udisks::Result& result) {
            if (alive.expired())
                return;
            rehold();
            Volume& volume = m_volumes[m_index];
            if (result.ok()) {
                runCheck();
                return;
            }
            // Nothing was unmounted, so nothing is mounted again.
            volume.wasMounted = false;
            if (stopOnCancel(result)) {
                nextVolume();
                return;
            }
            if (udisks::isBusyError(result.errorName, result.errorMessage)) {
                const std::vector<std::string> holders = udisks::processesUsing(mountPoints);
                std::string names;
                for (const std::string& holder : holders)
                    names += (names.empty() ? "“" : ", “") + holder + "”";
                log(holders.empty()
                            ? format(tr("“{name}” is in use and can’t be unmounted. Close the files and quit the "
                                        "programs using it, then run First Aid again."),
                                     {{"name", volume.name}})
                            : format(tr("“{name}” is in use by {programs} and can’t be unmounted. Close their files "
                                        "on it or quit them, then run First Aid again."),
                                     {{"name", volume.name}, {"programs", names}}));
            } else {
                log(format(tr("“{name}” could not be unmounted: {message}"),
                           {{"name", volume.name}, {"message", result.errorMessage}}));
            }
            ++m_failed;
            nextVolume();
        });
        heldThrough();
    });
}

void FirstAid::runCheck()
{
    log(tr("Checking the file system."));
    updateProgress();
    release();
    m_context.operations.check(m_volumes[m_index].block, [this, alive = std::weak_ptr<bool>(m_alive)](
                                                                const udisks::Result& result) {
        if (alive.expired())
            return;
        rehold();
        if (!result.ok()) {
            if (!stopOnCancel(result)) {
                log(format(tr("The check failed: {message}"), {{"message", result.errorMessage}}));
                ++m_failed;
            }
            remount();
            return;
        }
        if (result.boolean()) {
            log(tr("The file system appears to be OK."));
            remount();
            return;
        }
        log(tr("The file system has errors."));
        repairVolume();
    });
    heldThrough();
}

void FirstAid::repairVolume()
{
    const std::string type = m_volumes[m_index].type;
    m_context.operations.canRepair(type, [this, type, alive = std::weak_ptr<bool>(m_alive)](
                                                 bool available, const std::string& missing) {
        if (alive.expired())
            return;
        if (!available) {
            log(missing.empty()
                        ? format(tr("UDisks2 could not tell whether {format} volumes can be repaired."),
                                 {{"format", udisks::filesystemName(type)}})
                        : format(tr("{format} volumes can’t be repaired: {tool} is not installed."),
                                 {{"format", udisks::filesystemName(type)}, {"tool", missing}}));
            ++m_unrepaired;
            remount();
            return;
        }
        // The check may have taken long enough for the disk to change.
        if (!stillThere()) {
            ++m_failed;
            nextVolume();
            return;
        }
        log(tr("Repairing the file system."));
        setStatus(format(tr("Repairing “{name}”…"), {{"name", m_volumes[m_index].name}}));
        release();
        m_context.operations.repair(m_volumes[m_index].block, [this, alive](const udisks::Result& result) {
            if (alive.expired())
                return;
            rehold();
            if (result.ok() && result.boolean()) {
                log(tr("The file system was repaired."));
            } else if (result.ok()) {
                log(tr("The file system could not be repaired."));
                ++m_unrepaired;
            } else if (!stopOnCancel(result)) {
                log(format(tr("The repair failed: {message}"), {{"message", result.errorMessage}}));
                ++m_unrepaired;
            }
            remount();
        });
        heldThrough();
    });
}

void FirstAid::remount()
{
    Volume& volume = m_volumes[m_index];
    if (!volume.wasMounted) {
        nextVolume();
        return;
    }
    if (!stillThere()) {
        ++m_failed;
        nextVolume();
        return;
    }
    log(format(tr("Mounting “{name}” again."), {{"name", volume.name}}));
    release();
    m_context.operations.mount(volume.block, [this, alive = std::weak_ptr<bool>(m_alive)](
                                                     const udisks::Result& result) {
        if (alive.expired())
            return;
        rehold();
        if (!result.ok() && !result.cancelled()) {
            log(format(tr("“{name}” could not be mounted again: {message}"),
                       {{"name", m_volumes[m_index].name}, {"message", result.errorMessage}}));
            ++m_failed;
        }
        nextVolume();
    });
    heldThrough();
}

bool FirstAid::stopOnCancel(const udisks::Result& result)
{
    if (!result.cancelled())
        return false;
    if (!m_cancelled)
        log(tr("First Aid was cancelled."));
    m_cancelled = true;
    return true;
}

void FirstAid::finish()
{
    m_running = false;
    std::string summary;
    if (m_cancelled)
        summary = tr("First Aid was cancelled.");
    else if (m_unrepaired > 0)
        summary = tr("First Aid found errors it could not repair. Back up the data you can and erase the volume.");
    else if (m_failed > 0)
        summary = tr("First Aid process has failed.");
    else if (m_skipped > 0)
        summary = tr("First Aid process has completed.") + std::string(" ")
                + format(properdesktop::i18n::trn("{n} volume was skipped.", "{n} volumes were skipped.",
                                             uint64_t(m_skipped)),
                         {{"n", std::to_string(m_skipped)}});
    else
        summary = tr("First Aid process has completed.");
    m_hold.reset();
    log(summary);
    if (m_dialog && m_dialog->window()) {
        setStatus(summary);
        m_progress->setIndeterminate(false);
        m_progress->setValue(1);
        // The log tells what went wrong.
        if (m_failed > 0 || m_unrepaired > 0)
            setDetailsShown(true);
    } else {
        m_context.toast(summary);
    }
    m_subscriptions.clear();
}

void FirstAid::log(const std::string& line)
{
    m_log += (m_log.empty() ? "" : "\n") + line;
    if (m_logView && m_dialog && m_dialog->window())
        m_logView->setText(m_log);
}

void FirstAid::setStatus(const std::string& status)
{
    if (m_status && m_dialog && m_dialog->window())
        m_status->setText(status);
}

void FirstAid::updateProgress()
{
    if (!m_running || !m_progress || !m_dialog || !m_dialog->window())
        return;
    for (const Object* job : m_context.model.jobsFor(m_volumes[m_index].block)) {
        if (job->job && job->job->progressValid) {
            m_progress->setIndeterminate(false);
            m_progress->setValue(job->job->progress);
            return;
        }
    }
    m_progress->setIndeterminate(true);
}

void FirstAid::showDialog()
{
    m_dialog = std::make_unique<DialogWindow>(&m_context.window, tr("First Aid"), kWidth);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });
    // Closed while it runs: the run goes on and its result shows as a toast.
    m_dialog->onClosed = [this] {
        m_status = nullptr;
        m_progress = nullptr;
        m_logView = nullptr;
        m_details = nullptr;
        m_disclosure = nullptr;
    };
    auto* form = m_dialog->setContent<Form>();
    m_status = form->add<Label>();
    m_status->setName("firstAid:status");
    m_status->setWrap(true);
    m_progress = form->add<ProgressBar>();
    m_progress->setName("firstAid:progress");
    m_progress->setIndeterminate(true);
    auto* disclosure = form->add<Disclosure>();
    disclosure->setName("firstAid:details");
    m_disclosure = disclosure;
    disclosure->onClicked = [this] { setDetailsShown(!m_detailsShown); };
    auto* details = form->add<LogCard>();
    m_details = details;
    m_logView = details->log;
    m_logView->setText(m_log);
    setDetailsShown(false);
    m_subscriptions.clear();
    m_subscriptions.push_back(m_context.model.changed.connect([this] { updateProgress(); }));
    m_dialog->show();
}

void FirstAid::setDetailsShown(bool shown)
{
    m_detailsShown = shown;
    if (!m_details)
        return;
    m_details->setVisible(shown);
    m_disclosure->setOpen(shown);
    if (m_dialog && m_dialog->window())
        m_dialog->window()->requestLayout();
}

} // namespace dialogs
