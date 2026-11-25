// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/mount.h"

#include "dialogs/unlock.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/errors.h"
#include "udisks/holders.h"
#include "udisks/model.h"
#include "udisks/operations.h"

#include "base/event_loop.h"
#include "base/i18n.h"

using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using udisks::Object;

namespace dialogs {

Mounting::Mounting(views::Context& context)
    : m_context(context)
{
    ActionDispatcher& actions = context.actions;
    actions.setHandler(Action::Mount, [this](const Selection& selection) { mount(selection); });
    actions.setHandler(Action::Unmount, [this](const Selection& selection) { unmount(selection); });
    actions.setHandler(Action::Lock, [this](const Selection& selection) { lock(selection); });
    actions.setHandler(Action::Eject, [this](const Selection& selection) { eject(selection); });
}

Mounting::~Mounting()
{
    ActionDispatcher& actions = m_context.actions;
    for (Action action : {Action::Mount, Action::Unmount, Action::Lock, Action::Eject})
        actions.setHandler(action, {});
}

static bool isMounted(const Object* block)
{
    return block && block->filesystem && !block->filesystem->mountPoints.empty();
}

std::string volumeName(const views::Context& context, const std::string& block)
{
    const Object* object = context.model.object(block);
    std::string id = "v:" + block;
    if (object && object->block && !object->block->cryptoBackingDevice.empty())
        id = "v:" + object->block->cryptoBackingDevice;
    if (const udisks::Node* node = udisks::findNode(context.tree, id))
        return udisks::displayName(context.model, *node);
    if (object && object->block && !object->block->idLabel.empty())
        return object->block->idLabel;
    return udisks::deviceName(context.model, block);
}

std::vector<std::string> Mounting::mountPointsOf(const std::string& block) const
{
    std::vector<std::string> mountPoints;
    const auto add = [&](const Object* object) {
        if (object && object->filesystem)
            mountPoints.insert(mountPoints.end(), object->filesystem->mountPoints.begin(),
                               object->filesystem->mountPoints.end());
    };
    const Object* object = m_context.model.object(block);
    if (object && object->partitionTable) {
        for (const Object* volume : volumesOnDisk(m_context.model, block))
            add(volume);
    }
    add(m_context.model.contentOf(block));
    if (object && object != m_context.model.contentOf(block))
        add(object);
    return mountPoints;
}

void Mounting::mount(const Selection& selection)
{
    if (!selection)
        return;
    const udisks::Model& model = m_context.model;
    std::vector<Step> steps;
    const auto add = [&](const Object* volume) {
        if (!volume || !volume->block)
            return;
        if (volume->encrypted && !model.cleartextOf(volume->path))
            steps.push_back({Kind::Unlock, volume->path, volumeName(m_context, volume->path)});
        else if (volume->filesystem && !isMounted(volume))
            steps.push_back({Kind::Mount, volume->path, volumeName(m_context, volume->path)});
    };
    if (selection->kind == udisks::NodeKind::Disk) {
        for (const Object* volume : volumesOnDisk(model, selection->disk))
            add(volume);
    } else {
        add(model.contentOf(selection->block));
    }
    start(std::move(steps));
}

void Mounting::unmountSteps(const Object* volume, std::vector<Step>& steps) const
{
    if (!volume || !volume->block)
        return;
    const udisks::Model& model = m_context.model;
    const Object* content = model.contentOf(volume->path);
    // A cleartext block, as a disk's volumes list it.
    if (!volume->block->cryptoBackingDevice.empty()) {
        content = volume;
        volume = model.object(volume->block->cryptoBackingDevice);
        if (!volume)
            return;
    }
    if (isMounted(content))
        steps.push_back({Kind::Unmount, content->path, volumeName(m_context, content->path)});
    if (volume->encrypted && content && content != volume)
        steps.push_back({Kind::Lock, volume->path, volumeName(m_context, volume->path)});
}

void Mounting::unmount(const Selection& selection)
{
    if (!selection)
        return;
    std::vector<Step> steps;
    if (selection->kind == udisks::NodeKind::Disk) {
        for (const Object* volume : volumesOnDisk(m_context.model, selection->disk))
            unmountSteps(volume, steps);
    } else {
        unmountSteps(m_context.model.object(selection->block), steps);
    }
    start(std::move(steps));
}

void Mounting::lock(const Selection& selection)
{
    if (!selection)
        return;
    std::vector<Step> steps;
    unmountSteps(m_context.model.object(selection->block), steps);
    start(std::move(steps));
}

void Mounting::eject(const Selection& selection)
{
    if (!selection)
        return;
    udisks::Node disk;
    disk.kind = udisks::NodeKind::Disk;
    disk.id = "d:" + selection->disk;
    disk.drive = selection->drive;
    disk.block = selection->disk;
    disk.disk = selection->disk;
    start({{Kind::Eject, selection->disk, udisks::displayName(m_context.model, disk)}});
}

void Mounting::start(std::vector<Step> steps)
{
    // A run still going keeps its turn; the window's actions are disabled
    // while it is busy anyway.
    if (m_next < m_steps.size())
        return;
    m_steps = std::move(steps);
    m_next = 0;
    next();
}

void Mounting::next()
{
    if (m_next >= m_steps.size()) {
        m_steps.clear();
        m_next = 0;
        return;
    }
    const Step step = m_steps[m_next++];
    const std::vector<std::string> mountPoints = mountPointsOf(step.block);
    const auto done = [this, step, mountPoints, alive = std::weak_ptr<bool>(m_alive)](const udisks::Result& result) {
        if (alive.expired())
            return;
        if (!result.ok()) {
            failed(step, result, mountPoints);
            m_steps.clear();
            m_next = 0;
            return;
        }
        next();
    };
    udisks::Operations& operations = m_context.operations;
    switch (step.kind) {
    case Kind::Unlock: {
        m_unlock = std::make_unique<UnlockDialog>(m_context, step.block, step.name);
        m_unlock->onUnlocked = [this, step](const std::string& cleartext) {
            m_steps.insert(m_steps.begin() + long(m_next), {Kind::Mount, cleartext, step.name});
        };
        // The next step may replace the dialog, which is still closing.
        m_unlock->onClosed = [this, alive = std::weak_ptr<bool>(m_alive)] {
            properdesktop::base::EventLoop::current()->post([this, alive] {
                if (!alive.expired())
                    next();
            });
        };
        m_unlock->show();
        return;
    }
    case Kind::Mount:
        operations.mount(step.block, done);
        return;
    case Kind::Unmount:
        operations.unmount(step.block, done);
        return;
    case Kind::Lock:
        operations.lock(step.block, done);
        return;
    case Kind::Eject:
        operations.ejectDisk(step.block, done);
        return;
    }
}

static std::string quotedList(const std::vector<std::string>& names)
{
    std::string text;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0)
            text += i + 1 == names.size() ? tr(" and ") : ", ";
        text += "“" + names[i] + "”";
    }
    return text;
}

void Mounting::failed(const Step& step, const udisks::Result& result, const std::vector<std::string>& mountPoints)
{
    // The window says why a refusal happened; a dismissed password prompt
    // needs nothing said.
    if (result.cancelled() || result.errorName == udisks::Operations::kSystemError)
        return;
    std::string title;
    switch (step.kind) {
    case Kind::Unlock:
    case Kind::Mount:
        title = format(tr("“{name}” couldn’t be mounted."), {{"name", step.name}});
        break;
    case Kind::Unmount:
        title = format(tr("“{name}” couldn’t be unmounted."), {{"name", step.name}});
        break;
    case Kind::Lock:
        title = format(tr("“{name}” couldn’t be locked."), {{"name", step.name}});
        break;
    case Kind::Eject:
        title = format(tr("“{name}” couldn’t be ejected."), {{"name", step.name}});
        break;
    }
    std::string message = result.errorMessage;
    if (udisks::isBusyError(result.errorName, result.errorMessage)) {
        const std::vector<std::string> holders = udisks::processesUsing(mountPoints);
        if (holders.empty())
            message = tr("A program is still using a file on it. Close the files and quit the programs using it, "
                         "then try again.");
        else
            message = format(tr("It is in use by {programs}. Close their files on it or quit them, then try again."),
                             {{"programs", quotedList(holders)}});
    } else if (result.errorName == udisks::Operations::kBusyError) {
        message = tr("Another operation on this disk is still running.");
    }
    m_context.showError(title, message);
}

} // namespace dialogs
