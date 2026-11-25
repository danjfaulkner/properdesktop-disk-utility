// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "dialogs/identity_guard.h"
#include "dialogs/partition_plan.h"
#include "dialogs/volume_format.h"

#include "base/signal.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class TextField;
}

namespace views {
struct Context;
}

namespace dialogs {

class ChoiceButton;
class Footnote;
class PlanRunner;

// Adds a partition in the disk's largest free space: its name, format and
// size, from the start of that space.
class AddPartitionDialog
{
public:
    AddPartitionDialog(views::Context& context, std::string disk);
    ~AddPartitionDialog();

    AddPartitionDialog(const AddPartitionDialog&) = delete;
    AddPartitionDialog& operator=(const AddPartitionDialog&) = delete;

    void show();
    bool isOpen() const;
    // Where the partition goes; empty when there is no room.
    const FreeRegion& region() const { return m_region; }
    // The step Add runs.
    PlanStep step() const;
    std::string problem() const;

private:
    void updateFormats();
    void update();
    void addPartition();
    void finished();

    views::Context& m_context;
    std::string m_disk;
    // The disk as the dialog opened on it, checked again before anything.
    udisks::Identity m_identity;
    std::string m_tableType;
    FreeRegion m_region;
    std::string m_refusal;
    FormatAvailability m_formats;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<PlanRunner> m_runner;
    std::unique_ptr<IdentityGuard> m_guard;
    properdesktop::ui::TextField* m_name = nullptr;
    ChoiceButton* m_format = nullptr;
    properdesktop::ui::TextField* m_size = nullptr;
    uint64_t m_chosenSize = 0;
    bool m_sizeTyped = false;
    Footnote* m_note = nullptr;
    Footnote* m_problem = nullptr;
    properdesktop::ui::DialogButton* m_add = nullptr;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
};

// Sets the handlers of Erase, Partition, Add Partition and Delete
// Partition on the context's actions. Their dialogs stay with the handlers.
void installVolumeActions(views::Context& context);

} // namespace dialogs
