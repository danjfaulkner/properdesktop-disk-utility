// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "actions.h"
#include "dialogs/identity_guard.h"
#include "udisks/identity.h"
#include "udisks/operations.h"

#include "base/signal.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace properdesktop::ui {
class ConfirmDialog;
class DialogWindow;
class Label;
class ProgressBar;
class TextArea;
class View;
}

namespace udisks {
struct Result;
}

namespace views {
struct Context;
}

namespace dialogs {

class Disclosure;

// First Aid: after a confirmation, checks the volume, or every volume of a
// disk in turn, and repairs what the check finds. A mounted volume is
// unmounted for it and mounted again afterwards. The dialog shows the step
// running and, under Show Details, the log of every step. A filesystem
// whose checking tool is missing is disabled once UDisks2 has said so.
class FirstAid
{
public:
    explicit FirstAid(views::Context& context);
    ~FirstAid();

    FirstAid(const FirstAid&) = delete;
    FirstAid& operator=(const FirstAid&) = delete;

    void start(const Selection& selection);
    bool isRunning() const { return m_running; }

private:
    struct Volume
    {
        // As listed: the filesystem's block, or the encrypted one.
        std::string entry;
        std::string name;
        // Found again from the model when its turn comes: the block that
        // carries the filesystem (the cleartext one when encrypted), its
        // IdType and whether it was mounted.
        std::string block;
        std::string type;
        bool wasMounted = false;
        // The entry as the run listed it; another one at its path is skipped.
        udisks::Identity identity;
    };
    enum class Found {
        Ready,
        Gone,
        Locked,
        // Unlocked, but what it holds is no filesystem (LVM, say).
        NoFilesystem,
    };
    enum class Outcome {
        Completed,
        Unrepaired,
        Failed,
        Cancelled,
    };

    ActionState refine(const Selection& selection) const;
    std::vector<Volume> volumesOf(const udisks::Node& node) const;
    Found resolve(Volume& volume) const;
    void run(const udisks::Node& node);
    // The disk stays busy for the whole run; each call of ours goes through
    // the hold, keeping the disk busy itself until it returns.
    void release();
    void rehold();
    // After a call: holds the disk again when the call did not take it.
    void heldThrough();
    // Finds the volume again before the next step on it, its path and
    // identity; says so when it is gone.
    bool stillThere();
    void nextVolume();
    void checkVolume();
    void runCheck();
    void repairVolume();
    void remount();
    // A step failed: the run goes on with the next volume, or stops when
    // the password prompt was dismissed.
    bool stopOnCancel(const udisks::Result& result);
    void log(const std::string& line);
    void setStatus(const std::string& status);
    void finish();
    void showDialog();
    void setDetailsShown(bool shown);
    void updateProgress();

    views::Context& m_context;
    // A filesystem's checking tool that is not installed, by IdType.
    std::map<std::string, std::string> m_missingTools;

    std::vector<Volume> m_volumes;
    size_t m_index = 0;
    bool m_running = false;
    bool m_cancelled = false;
    // This volume's check or repair did not leave it consistent.
    bool m_problem = false;
    int m_failed = 0;
    int m_skipped = 0;
    int m_unrepaired = 0;
    std::string m_log;
    std::string m_disk;
    udisks::Operations::Hold m_hold;

    std::unique_ptr<properdesktop::ui::ConfirmDialog> m_confirm;
    std::unique_ptr<IdentityGuard> m_confirmGuard;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    properdesktop::ui::Label* m_status = nullptr;
    properdesktop::ui::ProgressBar* m_progress = nullptr;
    properdesktop::ui::View* m_details = nullptr;
    properdesktop::ui::TextArea* m_logView = nullptr;
    Disclosure* m_disclosure = nullptr;
    bool m_detailsShown = false;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace dialogs
