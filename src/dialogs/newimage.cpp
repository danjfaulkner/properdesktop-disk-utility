// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/newimage.h"
#include "dialogs/cards.h"
#include "dialogs/choice_button.h"
#include "dialogs/volume_format.h"

#include "images/imagefile.h"
#include "images/size.h"
#include "udisks/describe.h"
#include "views/context.h"

#include "base/i18n.h"
#include "base/paths.h"
#include "ui/button.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/label.h"
#include "ui/progress_bar.h"
#include "ui/text_field.h"
#include "ui/window.h"

#include <algorithm>
#include <filesystem>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 460;
static constexpr double kFieldWidth = 220;
static constexpr images::Scheme kSchemes[] = {images::Scheme::Gpt, images::Scheme::Mbr, images::Scheme::None};

static std::vector<ChoiceButton::Item> items(const std::vector<std::string>& texts)
{
    std::vector<ChoiceButton::Item> list;
    for (const std::string& text : texts)
        list.push_back({text, text});
    return list;
}

// The folder as the user knows it: ~ for the home folder.
static std::string shownFolder(const std::string& folder)
{
    const std::string home = properdesktop::base::paths::home();
    if (!home.empty() && (folder == home || folder.starts_with(home + "/")))
        return "~" + folder.substr(home.size());
    return folder;
}

static std::string defaultFolder()
{
    std::error_code error;
    const std::string documents = properdesktop::base::paths::userDirectory(properdesktop::base::paths::UserDirectory::Documents);
    if (std::filesystem::is_directory(documents, error))
        return documents;
    return properdesktop::base::paths::home();
}

NewImageDialog::NewImageDialog(views::Context& context, ChooseFolder chooseFolder)
    : m_context(context)
    , m_chooseFolder(std::move(chooseFolder))
    , m_folder(defaultFolder())
    , m_formats(std::make_unique<FormatAvailability>(context.operations))
{
    m_subscriptions.push_back(m_formats->changed.connect([this] {
        updateFormats();
        update();
    }));
}

NewImageDialog::~NewImageDialog() = default;

bool NewImageDialog::isOpen() const
{
    return m_dialog && m_dialog->window();
}

void NewImageDialog::show()
{
    if (isOpen()) {
        m_dialog->window()->activate();
        return;
    }
    m_error.clear();
    build();
    m_formats->query();
    update();
    m_dialog->show();
}

void NewImageDialog::setFolder(std::string folder)
{
    m_folder = std::move(folder);
    if (m_where)
        m_where->setText(shownFolder(m_folder));
    update();
}

static CardRow* addRow(Card* card, const std::string& text)
{
    return card->addChild<CardRow>(text);
}

void NewImageDialog::build()
{
    m_dialog = std::make_unique<DialogWindow>(&m_context.window, tr("New Blank Image"), kWidth);
    DialogWindow* dialog = m_dialog.get();
    m_dialog->window()->onCloseRequested = [this, dialog] {
        if (!isRunning())
            dialog->close();
    };
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this, dialog] {
        if (!isRunning())
            dialog->close();
    });
    m_dialog->onClosed = [this] {
        m_saveAs = m_name = m_size = m_password = m_verify = nullptr;
        m_where = nullptr;
        m_unit = m_format = m_scheme = m_encryption = nullptr;
        m_passwords = nullptr;
        m_bar = nullptr;
        m_note = nullptr;
        m_create = nullptr;
    };
    const auto changed = [this] {
        m_error.clear();
        update();
    };

    auto* form = m_dialog->setContent<Form>();
    Card* file = form->addCard();
    auto* saveAs = file->addChild<TextFieldRow>(tr("Save As"), kFieldWidth);
    m_saveAs = saveAs->field();
    m_saveAs->setName("newImage:saveAs");
    m_saveAs->setText(tr("Untitled") + std::string(".img"));
    m_saveAs->onTextChanged = changed;
    CardRow* where = addRow(file, tr("Where"));
    m_where = where->addContent<Label>(shownFolder(m_folder));
    m_where->setName("newImage:where");
    m_where->setColor(ColorId::DisabledText);
    m_where->setElide(Label::Elide::Middle);
    auto* choose = where->addContent<Button>(tr("Choose…"));
    choose->setName("newImage:chooseFolder");
    choose->onClicked = [this] {
        if (m_chooseFolder)
            m_chooseFolder(m_folder, [this](const std::string& folder) { setFolder(folder); });
    };

    Card* volume = form->addCard();
    auto* name = volume->addChild<TextFieldRow>(tr("Name"), kFieldWidth);
    m_name = name->field();
    m_name->setName("newImage:name");
    m_name->setText(defaultLabel(erasableFilesystems().front()));
    m_name->onTextChanged = changed;

    CardRow* size = addRow(volume, tr("Size"));
    m_size = size->addContent<TextField>("100");
    m_size->setName("newImage:size");
    m_size->setFrameVisible(false);
    m_size->setRightAligned(true);
    m_size->setTextMargins(0, 0);
    m_size->setLayoutHints({.preferredWidth = 120, .preferredHeight = metrics::rowHeight - 12});
    m_size->onTextChanged = changed;
    std::vector<std::string> units;
    for (images::SizeUnit unit : images::kSizeUnits)
        units.push_back(images::unitName(unit));
    m_unit = size->addContent<ChoiceButton>();
    m_unit->setName("newImage:unit");
    m_unit->setItems(items(units));
    m_unit->setCurrentIndex(0);
    m_unit->onActivated = [changed](int) { changed(); };

    m_format = addRow(volume, tr("Format"))->addContent<ChoiceButton>();
    m_format->setName("newImage:format");
    updateFormats();
    m_format->onActivated = [this, changed](int) {
        // A name left at the default follows the format's way of writing it.
        for (const std::string& type : erasableFilesystems()) {
            if (m_name->text() == defaultLabel(type)) {
                m_name->setText(defaultLabel(filesystem()));
                break;
            }
        }
        changed();
    };

    m_scheme = addRow(volume, tr("Scheme"))->addContent<ChoiceButton>();
    m_scheme->setName("newImage:scheme");
    m_scheme->setItems(items({udisks::schemeName("gpt"), udisks::schemeName("dos"), tr("None")}));
    m_scheme->setCurrentIndex(0);
    m_scheme->onActivated = [changed](int) { changed(); };

    m_encryption = addRow(volume, tr("Encryption"))->addContent<ChoiceButton>();
    m_encryption->setName("newImage:encryption");
    m_encryption->setItems(items({tr("None"), tr("Encrypted with a Password")}));
    m_encryption->setCurrentIndex(0);
    m_encryption->onActivated = [changed](int) { changed(); };

    m_passwords = form->addCard();
    auto* password = m_passwords->addChild<TextFieldRow>(tr("Password"), kFieldWidth);
    m_password = password->field();
    m_password->setName("newImage:password");
    m_password->setEchoMode(TextField::EchoMode::Password);
    m_password->onTextChanged = changed;
    auto* verify = m_passwords->addChild<TextFieldRow>(tr("Verify"), kFieldWidth);
    m_verify = verify->field();
    m_verify->setName("newImage:verify");
    m_verify->setEchoMode(TextField::EchoMode::Password);
    m_verify->onTextChanged = changed;
    m_verify->onAccepted = [this] { create(); };

    m_bar = form->add<ProgressBar>();
    m_bar->setName("newImage:progress");
    m_bar->setIndeterminate(true);
    m_bar->setVisible(false);
    m_note = form->add<Footnote>();
    m_note->setName("newImage:note");

    Footer* footer = form->addFooter();
    m_create = footer->addEnd<DialogButton>(tr("Create"), true);
    m_create->setName("newImage:create");
    m_create->onClicked = [this] { create(); };
}

void NewImageDialog::updateFormats()
{
    if (!m_format)
        return;
    std::vector<ChoiceButton::Item> list;
    for (const std::string& type : erasableFilesystems()) {
        ChoiceButton::Item item{formatName(type), type};
        item.enabled = m_formats->isAvailable(type);
        item.reason = m_formats->reason(type);
        list.push_back(std::move(item));
    }
    const std::string current = m_format->currentValue();
    m_format->setItems(std::move(list));
    m_format->setCurrentValue(current.empty() ? erasableFilesystems().front() : current);
}

std::string NewImageDialog::filesystem() const
{
    const std::string value = m_format ? m_format->currentValue() : std::string();
    return value.empty() ? erasableFilesystems().front() : value;
}

bool NewImageDialog::encrypted() const
{
    return m_encryption && m_encryption->currentIndex() == 1;
}

std::optional<images::BlankImage> NewImageDialog::image(std::string* problem) const
{
    const auto refuse = [problem](std::string reason) -> std::optional<images::BlankImage> {
        if (problem)
            *problem = std::move(reason);
        return std::nullopt;
    };
    if (!m_saveAs)
        return refuse({});

    const std::string fileName = images::imageFileName(m_saveAs->text());
    if (fileName.empty())
        return refuse(tr("Type a name for the image file."));
    std::error_code error;
    if (!std::filesystem::is_directory(m_folder, error))
        return refuse(tr("The folder is no longer there."));
    const std::string path = (std::filesystem::path(m_folder) / fileName).string();
    if (std::filesystem::exists(path, error))
        return refuse(format(tr("A file named “{name}” already exists in this folder."), {{"name", fileName}}));

    const std::string type = filesystem();
    const int unitIndex = std::max(0, m_unit->currentIndex());
    const uint64_t unit = images::unitBytes(images::kSizeUnits[size_t(unitIndex)]);
    const std::optional<uint64_t> typed = udisks::parseSize(m_size->text(), unit);
    if (!typed)
        return refuse(tr("Type a size, such as 100 MB."));
    const images::Scheme scheme = kSchemes[size_t(std::max(0, m_scheme->currentIndex()))];
    // A partition map keeps a MiB free before the partition and, on GPT, one after.
    const uint64_t smallest = std::max(images::kMinimumImageSize, minimumSize(type))
                            + (scheme == images::Scheme::None ? 0 : 2 * kMiB);
    // Whole MiB, for the loop device's sectors and the partitions' alignment.
    const uint64_t size = alignUp(*typed);
    if (size < smallest)
        return refuse(format(tr("A {format} image needs at least {size}."),
                             {{"format", formatName(type)}, {"size", udisks::formatSize(smallest)}}));

    if (!m_formats->isAvailable(type))
        return refuse(m_formats->reason(type));
    const std::string label = normalizedLabel(type, m_name->text());
    if (const std::string reason = labelProblem(type, label); !reason.empty())
        return refuse(reason);

    images::BlankImage image;
    if (encrypted()) {
        if (m_password->text().empty())
            return refuse(tr("Type a password for the image."));
        if (m_password->text() != m_verify->text())
            return refuse(tr("The passwords do not match."));
        image.passphrase = m_password->text();
    }
    image.path = path;
    image.size = size;
    image.filesystem = type;
    image.scheme = scheme;
    image.name = label;
    return image;
}

void NewImageDialog::update()
{
    if (!isOpen() || !m_create)
        return;
    m_passwords->setVisible(encrypted());
    if (isRunning())
        return;
    std::string problem;
    const bool ready = image(&problem).has_value();
    m_create->setEnabled(ready);
    if (!m_error.empty()) {
        m_note->setError(true);
        m_note->setText(m_error);
    } else {
        m_note->setError(false);
        m_note->setText(problem);
    }
}

void NewImageDialog::setWorking(bool working)
{
    for (View* view : std::initializer_list<View*>{m_saveAs, m_name, m_size, m_unit, m_format, m_scheme,
                                                   m_encryption, m_password, m_verify, m_create}) {
        view->setEnabled(!working);
    }
    m_bar->setVisible(working);
    m_dialog->setCloseVisible(!working);
}

void NewImageDialog::create()
{
    if (!isOpen() || isRunning())
        return;
    std::string problem;
    const std::optional<images::BlankImage> made = image(&problem);
    if (!made) {
        update();
        return;
    }
    m_error.clear();
    m_maker = std::make_unique<images::BlankImageMaker>(m_context.model, m_context.operations);
    m_maker->onStep = [this](images::BlankImageMaker::Step step) { this->step(step); };
    m_maker->onFinished = [this](const images::BlankImageMaker::Outcome& outcome) { finished(outcome); };
    setWorking(true);
    m_note->setError(false);
    m_maker->start(*made);
}

void NewImageDialog::step(images::BlankImageMaker::Step step)
{
    if (!m_note)
        return;
    switch (step) {
    case images::BlankImageMaker::Step::Creating:
        m_note->setText(tr("Creating the file…"));
        break;
    case images::BlankImageMaker::Step::Attaching:
        m_note->setText(tr("Attaching the image…"));
        break;
    case images::BlankImageMaker::Step::Partitioning:
        m_note->setText(tr("Creating the partition map…"));
        break;
    case images::BlankImageMaker::Step::Formatting:
        m_note->setText(tr("Formatting…"));
        break;
    case images::BlankImageMaker::Step::Mounting:
        m_note->setText(tr("Mounting…"));
        break;
    }
}

void NewImageDialog::finished(const images::BlankImageMaker::Outcome& outcome)
{
    if (!isOpen())
        return;
    setWorking(false);
    if (outcome.ok) {
        m_password->setText({});
        m_verify->setText({});
        m_dialog->close();
        if (onCreated)
            onCreated(outcome);
        return;
    }
    m_error = outcome.cancelled ? std::string() : outcome.error;
    update();
}

} // namespace dialogs
