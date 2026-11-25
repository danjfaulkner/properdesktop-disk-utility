// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "images/blankimage.h"

#include "base/signal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace properdesktop::ui {
class DialogButton;
class DialogWindow;
class Label;
class ProgressBar;
class TextField;
}

namespace views {
struct Context;
}

namespace dialogs {

class Card;
class ChoiceButton;
class FormatAvailability;
class Footnote;

// New Blank Image: the file's name and folder, the volume's name, size,
// format, partition map and encryption. Create makes the file, attaches
// it, partitions, formats and mounts it, the dialog showing each step and
// keeping an error until something is changed.
class NewImageDialog
{
public:
    // Asks for a folder; done with the path, or not at all when cancelled.
    using ChooseFolder = std::function<void(const std::string& current, std::function<void(const std::string&)> done)>;

    NewImageDialog(views::Context& context, ChooseFolder chooseFolder);
    ~NewImageDialog();

    NewImageDialog(const NewImageDialog&) = delete;
    NewImageDialog& operator=(const NewImageDialog&) = delete;

    void show();
    bool isOpen() const;
    bool isRunning() const { return m_maker && m_maker->isRunning(); }
    const std::string& folder() const { return m_folder; }
    void setFolder(std::string folder);

    // What Create would make, or why it cannot yet.
    std::optional<images::BlankImage> image(std::string* problem = nullptr) const;
    void create();

    // The image is made; the outcome names its loop device.
    std::function<void(const images::BlankImageMaker::Outcome& outcome)> onCreated;

private:
    void build();
    void update();
    void updateFormats();
    void setWorking(bool working);
    void step(images::BlankImageMaker::Step step);
    void finished(const images::BlankImageMaker::Outcome& outcome);
    std::string filesystem() const;
    bool encrypted() const;

    views::Context& m_context;
    ChooseFolder m_chooseFolder;
    std::string m_folder;
    std::unique_ptr<FormatAvailability> m_formats;
    std::unique_ptr<properdesktop::ui::DialogWindow> m_dialog;
    std::unique_ptr<images::BlankImageMaker> m_maker;
    // A failure stays shown until the form changes.
    std::string m_error;

    properdesktop::ui::TextField* m_saveAs = nullptr;
    properdesktop::ui::Label* m_where = nullptr;
    properdesktop::ui::TextField* m_name = nullptr;
    properdesktop::ui::TextField* m_size = nullptr;
    ChoiceButton* m_unit = nullptr;
    ChoiceButton* m_format = nullptr;
    ChoiceButton* m_scheme = nullptr;
    ChoiceButton* m_encryption = nullptr;
    Card* m_passwords = nullptr;
    properdesktop::ui::TextField* m_password = nullptr;
    properdesktop::ui::TextField* m_verify = nullptr;
    properdesktop::ui::ProgressBar* m_bar = nullptr;
    Footnote* m_note = nullptr;
    properdesktop::ui::DialogButton* m_create = nullptr;
    std::vector<properdesktop::base::Subscription> m_subscriptions;
};

} // namespace dialogs
