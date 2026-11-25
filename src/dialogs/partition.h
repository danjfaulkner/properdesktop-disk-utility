// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "dialogs/identity_guard.h"
#include "dialogs/partition_plan.h"
#include "dialogs/volume_format.h"
#include "udisks/tree.h"

#include "base/signal.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class HeaderButton;
class Label;
class ProgressBar;
class TextField;
}

namespace views {
struct Context;
}

namespace dialogs {

class ChoiceButton;
class Footnote;
class PieChart;
class PlanRunner;

// The partition editor of a whole disk: a pie of its partitions and free
// space, + and - under it, and the selected slice's name, format and size.
// Nothing changes on the disk until Apply, which lists the steps, then runs
// them one by one, stopping at the first that fails.
class PartitionDialog
{
public:
    PartitionDialog(views::Context& context, std::string disk);
    ~PartitionDialog();

    PartitionDialog(const PartitionDialog&) = delete;
    PartitionDialog& operator=(const PartitionDialog&) = delete;

    void show();
    bool isOpen() const;
    bool isRunning() const;

    const PartitionLayout& layout() const { return m_layout; }
    std::optional<uint32_t> selected() const { return m_selected; }
    void select(uint32_t id);
    // As a drag of the pie's handle does.
    void moveBoundary(size_t index, uint64_t position, bool finished = true);

private:
    void buildEditor();
    void refresh();
    void refreshFields();
    void refreshButtons();
    std::string newFilesystem() const;
    void add();
    void remove();
    void revert();
    void sizeEdited();
    void showSummary();
    void apply();
    void stepStarted(size_t index);
    void updateProgress();
    void stop();
    // The views of the content shown, which goes when it is replaced or the
    // window closes.
    void forgetViews();
    void finished();

    views::Context& m_context;
    std::string m_disk;
    std::string m_diskName;
    // The disk as the dialog opened on it, checked again before anything.
    udisks::Identity m_identity;
    PartitionLayout m_original;
    PartitionLayout m_layout;
    std::optional<uint32_t> m_selected;
    // The slice the size field was typed for, until it is applied.
    std::optional<uint32_t> m_sizeSlice;
    // Stop was clicked on the running step's job.
    bool m_stopped = false;
    FormatAvailability m_formats;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<PlanRunner> m_runner;
    std::unique_ptr<IdentityGuard> m_guard;

    PieChart* m_pie = nullptr;
    properdesktop::ui::HeaderButton* m_add = nullptr;
    properdesktop::ui::HeaderButton* m_remove = nullptr;
    properdesktop::ui::TextField* m_name = nullptr;
    ChoiceButton* m_format = nullptr;
    properdesktop::ui::TextField* m_size = nullptr;
    Footnote* m_note = nullptr;
    Footnote* m_problem = nullptr;
    properdesktop::ui::DialogButton* m_apply = nullptr;
    properdesktop::ui::DialogButton* m_revert = nullptr;

    properdesktop::ui::Label* m_status = nullptr;
    properdesktop::ui::ProgressBar* m_bar = nullptr;
    Footnote* m_stepNote = nullptr;
    Footnote* m_error = nullptr;
    properdesktop::ui::DialogButton* m_done = nullptr;
    properdesktop::ui::DialogButton* m_stop = nullptr;

    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
