// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "udisks/tree.h"

#include "base/signal.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace udisks {
class Model;
class Operations;
struct Object;
}

// What the window can do to the selection, from the toolbar, the menus and
// the sidebar's context menu. Each action is enabled when a handler is set
// for it and the selection allows it (actionState()).
enum class Action {
    // The toolbar.
    // Show Only Volumes or Show All Devices.
    View,
    // A partition in the selected disk's free space.
    AddVolume,
    // Deletes the selected partition.
    RemoveVolume,
    FirstAid,
    Partition,
    Erase,
    Restore,
    // Mounts the volume, unlocking it first when it is encrypted, or every
    // volume of a disk; the toolbar shows Unmount instead while that is
    // possible.
    Mount,
    // Unmounts the volume, and locks it when it is encrypted, or every
    // volume of a disk.
    Unmount,
    Info,

    // The menus and the sidebar.
    Eject,
    Rename,
    // Unmounts an unlocked encrypted volume and locks it.
    Lock,
    AttachImage,
    NewBlankImage,
    NewImageFromSelection,
};

inline constexpr std::array<Action, 10> kToolbarActions = {
    Action::View,    Action::AddVolume, Action::RemoveVolume, Action::FirstAid, Action::Partition,
    Action::Erase,   Action::Restore,   Action::Mount,        Action::Unmount,  Action::Info,
};

// The menu item's text: "Erase…", "Mount", "Get Info"...
std::string actionTitle(Action action);
// For view names: "view", "addVolume", "firstAid"...
std::string actionName(Action action);

// The sidebar's selection; none while nothing is selected.
using Selection = std::optional<udisks::Node>;

struct ActionState
{
    bool enabled = false;
    // Why it is disabled, for a tool tip; empty when there is nothing to say.
    std::string reason;

    bool operator==(const ActionState&) const = default;
};

// Whether the selection allows the action, whatever handlers are set. The
// running system's volumes and disk are never erased, partitioned, restored
// onto, repaired or unmounted; a busy disk (an operation of ours or a job of
// UDisks2's on it) takes no other change.
ActionState actionState(Action action, const udisks::Model& model, const udisks::Operations* operations,
                        const Selection& selection);

// The largest unpartitioned stretch of a disk with a partition table, in
// whole MiB from a MiB boundary, a MiB kept free at each end for the
// table, below 2 TiB on a Master Boot Record: what Add Volume can use. 0
// without a table.
uint64_t largestFreeSpace(const udisks::Model& model, const std::string& disk);
// The mountable volumes of a disk: their filesystem blocks (the cleartext
// block for an unlocked encrypted one) or locked encrypted blocks.
std::vector<const udisks::Object*> volumesOnDisk(const udisks::Model& model, const std::string& disk);

// The window's actions in one place: the dialogs and panes set the handlers
// for what they do, the toolbar, menus and sidebar ask for the state and
// trigger them.
class ActionDispatcher
{
public:
    using Handler = std::function<void(const Selection& selection)>;
    // A rule of the handler's own on top of actionState(), asked only while
    // that allows the action: a tool it needs is missing, say.
    using Refinement = std::function<ActionState(const Selection& selection)>;

    ActionDispatcher(udisks::Model& model, udisks::Operations& operations);

    ActionDispatcher(const ActionDispatcher&) = delete;
    ActionDispatcher& operator=(const ActionDispatcher&) = delete;

    void setHandler(Action action, Handler handler);
    bool hasHandler(Action action) const;
    void setRefinement(Action action, Refinement refinement);
    // What a refinement says has changed.
    void refresh() { statesChanged.emit(); }

    void setSelection(Selection selection);
    const Selection& selection() const { return m_selection; }

    // Disabled without a handler.
    ActionState state(Action action) const { return stateFor(action, m_selection); }
    ActionState stateFor(Action action, const Selection& selection) const;
    bool isEnabled(Action action) const { return state(action).enabled; }
    // Runs the handler with the selection when the action is enabled.
    bool trigger(Action action) { return triggerOn(action, m_selection); }
    // With another node, as a context menu does for the row it opened on,
    // whatever is selected by the time it is chosen.
    bool triggerOn(Action action, const Selection& selection);

    // The selection, the model or the busy state changed, or a handler was
    // set: every state may have changed.
    properdesktop::base::Signal<> statesChanged;

private:
    const udisks::Model& m_model;
    const udisks::Operations& m_operations;
    Selection m_selection;
    std::vector<std::pair<Action, Handler>> m_handlers;
    std::vector<std::pair<Action, Refinement>> m_refinements;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
};
