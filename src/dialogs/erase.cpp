// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/erase.h"

#include "dialogs/cards.h"
#include "dialogs/choice_button.h"
#include "dialogs/partition_plan.h"
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
#include "ui/menu.h"
#include "ui/progress_bar.h"
#include "ui/switch.h"
#include "ui/text_field.h"
#include "ui/window.h"

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 440;
static constexpr double kFieldWidth = 200;

// The filesystem the dialog offers first: the one there now, else exFAT for
// something carried between computers and ext4 for the rest.
static std::string defaultFormat(const udisks::Model& model, const udisks::Node& node)
{
    const udisks::Object* content = model.contentOf(node.block);
    if (content && content->block) {
        const std::string type = content->block->idType;
        for (const std::string& known : erasableFilesystems()) {
            if (known == type)
                return type;
        }
    }
    if (!node.drive.empty() && udisks::sectionOfDrive(model, node.drive) == udisks::Section::External)
        return "exfat";
    return "ext4";
}

EraseDialog::EraseDialog(views::Context& context, udisks::Node node)
    : m_context(context)
    , m_node(std::move(node))
    , m_formats(context.operations)
{
    m_name = udisks::displayName(context.model, m_node);
    const udisks::Object* object = context.model.object(m_node.block);
    m_size = object && object->block ? object->block->size : 0;
    m_defaultFormat = defaultFormat(context.model, m_node);

    m_dialog = std::make_unique<DialogWindow>(&context.window, format(tr("Erase “{name}”"), {{"name", m_name}}), kWidth);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->onCloseRequested = [this] {
        if (!isRunning())
            m_dialog->close();
    };
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] {
        if (!isRunning())
            m_dialog->close();
    });
    m_identity = udisks::identityOf(context.model, m_node.block);
    m_guard = std::make_unique<IdentityGuard>(context, std::vector<udisks::Identity>{m_identity}, m_name, *m_dialog,
                                              [this] { return isRunning(); });
    // Nothing of the window is touched once it is gone.
    m_dialog->onClosed = [this] {
        m_guard->disarm();
        m_subscriptions.clear();
        forgetViews();
    };
    m_subscriptions.push_back(m_formats.changed.connect([this] { updateFormats(); }));
    m_subscriptions.push_back(context.model.changed.connect([this] { updateProgress(); }));
    buildForm();
    m_formats.query();
}

EraseDialog::~EraseDialog() = default;

void EraseDialog::show()
{
    m_dialog->show();
    if (m_label)
        m_label->setFocus();
}

bool EraseDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

bool EraseDialog::isRunning() const
{
    return m_runner && m_runner->isRunning();
}

void EraseDialog::buildForm()
{
    auto* form = m_dialog->setContent<Form>();
    const bool wholeDisk = erasesWholeDisk(m_node);
    auto* message = form->add<Label>(
        wholeDisk ? format(tr("Erasing “{name}” deletes every volume on it and everything stored on them. This can't "
                              "be undone. Choose a name, a format and a partition scheme for it."),
                           {{"name", m_name}})
                  : format(tr("Erasing “{name}” deletes everything stored on it. This can't be undone. Choose a "
                              "name and a format for it."),
                           {{"name", m_name}}));
    message->setWrap(true);
    message->setName("erase:explanation");

    Card* card = form->addCard();
    m_label = card->addChild<TextFieldRow>(tr("Name"), kFieldWidth)->field();
    m_label->setName("erase:name");
    m_label->setText(defaultLabel(m_defaultFormat));
    m_label->onTextChanged = [this] {
        // FAT keeps names in capitals; the field shows them so.
        const std::string shown = normalizedLabel(m_format ? m_format->currentValue() : std::string(), m_label->text());
        if (shown != m_label->text()) {
            const size_t cursor = m_label->cursorPosition();
            m_label->setText(shown);
            m_label->setCursorPosition(cursor);
            return;
        }
        update();
    };
    m_label->onAccepted = [this] { erase(); };

    m_format = card->addChild<CardRow>(tr("Format"))->addContent<ChoiceButton>();
    m_format->setName("erase:format");
    m_format->onActivated = [this](int) {
        // A name still at its default follows the format's capitals.
        bool isDefault = false;
        for (const std::string& type : erasableFilesystems())
            isDefault = isDefault || m_label->text() == defaultLabel(type);
        const std::string type = m_format->currentValue();
        m_label->setText(isDefault ? defaultLabel(type) : normalizedLabel(type, m_label->text()));
        update();
    };

    if (wholeDisk) {
        m_scheme = card->addChild<CardRow>(tr("Scheme"))->addContent<ChoiceButton>();
        m_scheme->setName("erase:scheme");
        m_scheme->setItems({{udisks::schemeName("gpt"), "gpt"}, {udisks::schemeName("dos"), "dos"}});
        const udisks::Object* object = m_context.model.object(m_node.block);
        if (object && object->partitionTable)
            m_scheme->setCurrentValue(object->partitionTable->type);
        m_scheme->onActivated = [this](int) { update(); };
        m_schemeNote = form->add<Footnote>();
        m_schemeNote->setName("erase:schemeNote");
    }

    Card* encryption = form->addCard();
    m_encrypt = encryption->addChild<CardRow>(tr("Encrypt"))->addContent<Switch>();
    m_encrypt->setName("erase:encrypt");
    m_encrypt->setPadding({0, 0, 0, 0});
    m_encrypt->onToggled = [this](bool) { update(); };
    m_passphraseRow = encryption->addChild<TextFieldRow>(tr("Passphrase"), kFieldWidth);
    m_passphraseRow->field()->setName("erase:passphrase");
    m_passphraseRow->field()->setEchoMode(TextField::EchoMode::Password);
    m_passphraseRow->field()->setPlaceholder(tr("Required"));
    m_passphraseRow->field()->onTextChanged = [this] { update(); };
    m_verifyRow = encryption->addChild<TextFieldRow>(tr("Verify"), kFieldWidth);
    m_verifyRow->field()->setName("erase:verify");
    m_verifyRow->field()->setEchoMode(TextField::EchoMode::Password);
    m_verifyRow->field()->setPlaceholder(tr("Required"));
    m_verifyRow->field()->onTextChanged = [this] { update(); };
    m_verifyRow->field()->onAccepted = [this] { erase(); };
    form->add<Footnote>(tr("The volume can only be read after unlocking it with the passphrase. If you forget the "
                           "passphrase, the data can't be recovered."))
        ->setName("erase:encryptNote");

    m_securityNote = form->add<Footnote>();
    m_securityNote->setName("erase:securityNote");
    m_problem = form->add<Footnote>();
    m_problem->setName("erase:problem");
    m_problem->setError(true);

    Footer* footer = form->addFooter();
    m_securityButton = footer->addStart<DialogButton>(tr("Security Options…"));
    m_securityButton->setName("erase:security");
    m_securityButton->onClicked = [this] { showSecurityOptions(); };
    m_eraseButton = footer->addEnd<DialogButton>(tr("Erase"), true);
    m_eraseButton->setName("erase:erase");
    m_eraseButton->setDestructive(true);
    m_eraseButton->onClicked = [this] { erase(); };

    updateFormats();
}

void EraseDialog::updateFormats()
{
    if (!m_format)
        return;
    std::vector<ChoiceButton::Item> items;
    for (const std::string& type : erasableFilesystems()) {
        ChoiceButton::Item item{formatName(type), type};
        item.enabled = m_formats.isAvailable(type);
        item.reason = m_formats.reason(type);
        if (item.enabled && m_size > 0 && m_size < minimumSize(type)) {
            item.enabled = false;
            item.reason = format(tr("needs at least {size}"), {{"size", udisks::formatSize(minimumSize(type))}});
        }
        items.push_back(std::move(item));
    }
    const std::string current = m_format->currentValue();
    m_format->setItems(std::move(items));
    // Until the answers arrive the default stays chosen; then the first
    // filesystem that can be made replaces one that cannot.
    m_format->setCurrentValue(current.empty() ? m_defaultFormat : current);
    const auto& choices = m_format->items();
    const int index = m_format->currentIndex();
    const bool known = m_formats.entry(m_format->currentValue()).state != FormatAvailability::State::Unknown;
    if (index >= 0 && known && !choices[size_t(index)].enabled) {
        for (size_t i = 0; i < choices.size(); ++i) {
            if (choices[i].enabled) {
                m_format->setCurrentIndex(int(i));
                break;
            }
        }
    }
    update();
}

EraseRequest EraseDialog::request() const
{
    EraseRequest request;
    if (!m_format)
        return request;
    request.name = m_label->text();
    request.filesystem = m_format->currentValue();
    if (m_scheme)
        request.scheme = m_scheme->currentValue();
    if (m_encrypt->isChecked()) {
        request.passphrase = m_passphraseRow->field()->text();
        request.encryptionType = m_context.model.manager().defaultEncryptionType;
    }
    request.zeroFill = m_zeroFill;
    return request;
}

std::string EraseDialog::problem() const
{
    if (!m_format)
        return {};
    const std::string type = m_format->currentValue();
    const int index = m_format->currentIndex();
    // Until UDisks2 answers, Erase waits without a complaint.
    const bool known = m_formats.entry(type).state != FormatAvailability::State::Unknown;
    if (index >= 0 && known && !m_format->items()[size_t(index)].enabled)
        return format(tr("{format} can't be chosen: {reason}"),
                      {{"format", formatName(type)}, {"reason", m_format->items()[size_t(index)].reason}});
    const std::string label = labelProblem(type, m_label->text());
    if (!label.empty())
        return label;
    if (m_encrypt->isChecked()) {
        const std::string& passphrase = m_passphraseRow->field()->text();
        const std::string& verify = m_verifyRow->field()->text();
        if (!verify.empty() && passphrase != verify)
            return tr("The passphrases don't match.");
    }
    return {};
}

void EraseDialog::update()
{
    if (!m_format)
        return;
    const bool encrypt = m_encrypt->isChecked();
    m_passphraseRow->setVisible(encrypt);
    m_verifyRow->setVisible(encrypt);
    if (m_schemeNote) {
        const uint64_t reach = usableEnd(m_size, "dos");
        const bool cut = m_scheme->currentValue() == "dos" && m_size > reach + 2 * kMiB;
        m_schemeNote->setText(cut ? format(tr("A Master Boot Record reaches only the first {reach} of “{name}”; the "
                                              "other {rest} stay unused. Choose GUID Partition Map to use all of it."),
                                           {{"reach", udisks::formatSize(reach)},
                                            {"name", m_name},
                                            {"rest", udisks::formatSize(m_size - reach)}})
                                    : std::string());
    }
    m_securityNote->setText(m_zeroFill ? tr("The data is overwritten with zeros first, which takes as long as "
                                            "writing the whole volume once.")
                                       : std::string());
    const std::string why = problem();
    m_problem->setText(why);
    const bool passphraseReady = !encrypt
            || (!m_passphraseRow->field()->text().empty()
                && m_passphraseRow->field()->text() == m_verifyRow->field()->text());
    const bool formatKnown = m_formats.isAvailable(m_format->currentValue());
    m_eraseButton->setEnabled(why.empty() && passphraseReady && formatKnown);
}

void EraseDialog::showSecurityOptions()
{
    m_securityMenu = std::make_unique<Menu>();
    const size_t quick = m_securityMenu->addItem(tr("Quick Erase"), [this] {
        m_zeroFill = false;
        update();
    });
    const size_t zero = m_securityMenu->addItem(tr("Zero Out Data"), [this] {
        m_zeroFill = true;
        update();
    });
    m_securityMenu->setItemChecked(quick, !m_zeroFill);
    m_securityMenu->setItemChecked(zero, m_zeroFill);
    if (m_securityButton)
        m_securityMenu->popupFor(m_securityButton);
}

void EraseDialog::erase()
{
    if (isRunning() || !m_eraseButton || !m_eraseButton->isEnabled() || !m_context.model.matches(m_identity))
        return;
    m_lastRequest = request();
    std::vector<PlanStep> steps = eraseSteps(m_context.model, m_node, *m_lastRequest);
    if (steps.empty())
        return;
    m_stopped = false;
    m_runner = std::make_unique<PlanRunner>(m_context.model, m_context.operations, m_identity, std::move(steps));
    m_subscriptions.push_back(m_runner->stepStarted.connect([this](size_t index) { stepStarted(index); }));
    m_subscriptions.push_back(m_runner->finished.connect([this] { finished(); }));
    showProgress();
    m_runner->start();
}

void EraseDialog::forgetViews()
{
    m_label = nullptr;
    m_format = nullptr;
    m_scheme = nullptr;
    m_schemeNote = nullptr;
    m_encrypt = nullptr;
    m_passphraseRow = nullptr;
    m_verifyRow = nullptr;
    m_securityNote = nullptr;
    m_problem = nullptr;
    m_eraseButton = nullptr;
    m_securityButton = nullptr;
    m_status = nullptr;
    m_bar = nullptr;
    m_stepNote = nullptr;
    m_error = nullptr;
    m_done = nullptr;
    m_stop = nullptr;
}

void EraseDialog::showProgress()
{
    forgetViews();
    auto* form = m_dialog->setContent<Form>();
    m_status = form->add<Label>();
    m_status->setWrap(true);
    m_status->setName("erase:status");
    m_bar = form->add<ProgressBar>();
    m_bar->setName("erase:progress");
    m_bar->setLayoutHints({.fillWidth = true});
    m_bar->setIndeterminate(true);
    m_stepNote = form->add<Footnote>();
    m_stepNote->setName("erase:step");
    m_error = form->add<Footnote>();
    m_error->setName("erase:error");
    m_error->setError(true);
    Footer* footer = form->addFooter();
    m_stop = footer->addStart<DialogButton>(tr("Stop"));
    m_stop->setName("erase:stop");
    m_stop->setVisible(false);
    m_stop->onClicked = [this] { stop(); };
    m_done = footer->addEnd<DialogButton>(tr("Done"), true);
    m_done->setName("erase:done");
    m_done->setVisible(false);
    m_done->onClicked = [this] { m_dialog->close(); };
}

void EraseDialog::stepStarted(size_t index)
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

// UDisks2's job on the block: how far it is, and whether it can be stopped.
void EraseDialog::updateProgress()
{
    if (!isRunning() || !m_bar)
        return;
    const PlanStep& step = m_runner->steps()[m_runner->current()];
    const udisks::Object* job = jobOf(m_context.model, {step.path, m_node.disk});
    m_stop->setVisible(job && job->job->cancelable && !m_stopped);
    if (job && job->job->progressValid) {
        m_bar->setIndeterminate(false);
        m_bar->setValue(job->job->progress);
    }
}

// UDisks2 cancels the job where its tool allows; the step then fails as
// cancelled and nothing after it runs.
void EraseDialog::stop()
{
    if (!isRunning())
        return;
    const PlanStep& step = m_runner->steps()[m_runner->current()];
    const udisks::Object* job = jobOf(m_context.model, {step.path, m_node.disk});
    if (!job || !job->job->cancelable)
        return;
    m_stopped = true;
    m_stop->setVisible(false);
    m_status->setText(format(tr("Stopping “{step}”…"), {{"step", describe(step)}}));
    m_context.operations.cancelJob(job->path);
}

void EraseDialog::finished()
{
    if (!m_runner || !m_status)
        return;
    const udisks::Result& result = m_runner->result();
    // Turning down the password before anything changed goes back to the
    // form as it was filled in.
    if (dismissed(result) && m_runner->current() == 0) {
        forgetViews();
        buildForm();
        restore();
        return;
    }
    m_bar->setIndeterminate(false);
    m_stop->setVisible(false);
    m_done->setVisible(true);
    m_stepNote->setText({});
    if (m_runner->succeeded()) {
        m_bar->setValue(1);
        m_status->setText(format(tr("“{name}” was erased."), {{"name", m_name}}));
        m_done->setFocus();
        return;
    }
    m_bar->setVisible(false);
    const auto& steps = m_runner->steps();
    const size_t failed = m_runner->current();
    bool damaged = erasesData(steps[failed]) && ranPartWay(result);
    for (size_t i = 0; i < failed; ++i)
        damaged = damaged || erasesData(steps[i]);
    m_status->setText(damaged ? format(tr("“{name}” was erased only in part and has no usable volume now. Erase it "
                                          "again to use it."),
                                       {{"name", m_name}})
                              : format(tr("“{name}” was not erased; nothing stored on it was changed."),
                                       {{"name", m_name}}));
    const std::string step = describe(steps[m_runner->failedStep()]);
    if (m_stopped && result.cancelled())
        m_error->setText(format(tr("“{step}” was stopped before it finished."), {{"step", step}}));
    else if (result.cancelled())
        m_error->setText(format(tr("“{step}” was cancelled."), {{"step", step}}));
    else
        m_error->setText(result.errorMessage.empty()
                                 ? format(tr("“{step}” failed."), {{"step", step}})
                                 : format(tr("“{step}” failed: {message}"),
                                          {{"step", step}, {"message", result.errorMessage}}));
    m_done->setFocus();
}

void EraseDialog::restore()
{
    if (!m_lastRequest || !m_format)
        return;
    const EraseRequest& request = *m_lastRequest;
    m_format->setCurrentValue(request.filesystem);
    m_label->setText(request.name);
    if (m_scheme)
        m_scheme->setCurrentValue(request.scheme);
    m_encrypt->setChecked(!request.passphrase.empty());
    m_passphraseRow->field()->setText(request.passphrase);
    m_verifyRow->field()->setText(request.passphrase);
    update();
}

} // namespace dialogs
