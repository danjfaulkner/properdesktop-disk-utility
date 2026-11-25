// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "dialogs/smart.h"

#include "dialogs/cards.h"
#include "views/context.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/operations.h"
#include "udisks/properties.h"
#include "udisks/smart.h"

#include "base/event_loop.h"
#include "base/i18n.h"
#include "ui/color_provider.h"
#include "ui/dialog_window.h"
#include "ui/keys.h"
#include "ui/window.h"

#include <algorithm>

using namespace properdesktop::ui;
using properdesktop::i18n::format;
using properdesktop::i18n::tr;

namespace dialogs {

static constexpr double kWidth = 480;
static constexpr double kHeight = 600;

static std::string driveName(const udisks::Model& model, const std::string& drive)
{
    const udisks::Object* object = model.object(drive);
    if (!object || !object->drive)
        return tr("S.M.A.R.T.");
    std::string name = object->drive->vendor;
    if (!object->drive->model.empty())
        name += (name.empty() ? "" : " ") + object->drive->model;
    return name.empty() ? tr("S.M.A.R.T.") : name;
}

SmartDialog::SmartDialog(views::Context& context, Window* parent, std::string drive)
    : m_context(context)
    , m_drive(std::move(drive))
{
    m_dialog = std::make_unique<DialogWindow>(parent, driveName(context.model, m_drive), kWidth);
    m_dialog->setHeight(kHeight);
    m_dialog->setEdgeToEdge(true);
    m_dialog->window()->setBackgroundColor(ColorId::Background);
    m_dialog->window()->addShortcut(NoModifier, key::Escape, [this] { m_dialog->close(); });
    m_dialog->onClosed = [this] { m_subscriptions.clear(); };
    m_subscriptions.push_back(context.model.changed.connect([this] { schedule(); }));
    build();
    readAttributes();
}

SmartDialog::~SmartDialog() = default;

void SmartDialog::show()
{
    m_dialog->show();
}

bool SmartDialog::isOpen() const
{
    return m_dialog->window() != nullptr;
}

void SmartDialog::schedule()
{
    if (m_scheduled)
        return;
    m_scheduled = true;
    properdesktop::base::EventLoop::current()->post([this, alive = std::weak_ptr<bool>(m_alive)] {
        if (alive.expired())
            return;
        m_scheduled = false;
        if (!isOpen())
            return;
        if (!m_context.model.object(m_drive)) {
            m_dialog->close();
            return;
        }
        build();
    });
}

void SmartDialog::readAttributes()
{
    m_context.operations.smartGetAttributes(m_drive, [this, alive = std::weak_ptr<bool>(m_alive)](
                                                             const udisks::Result& result) {
        if (alive.expired() || !isOpen())
            return;
        if (result.ok()) {
            m_attributes = result.values;
            m_error.clear();
        } else if (!result.cancelled()) {
            m_error = format(tr("The attributes could not be read: {message}"), {{"message", result.errorMessage}});
        }
        build();
    });
}

void SmartDialog::startSelftest()
{
    if (m_starting)
        return;
    m_starting = true;
    m_error.clear();
    build();
    m_context.operations.smartSelftestStart(m_drive, "short", [this, alive = std::weak_ptr<bool>(m_alive)](
                                                                      const udisks::Result& result) {
        if (alive.expired() || !isOpen())
            return;
        m_starting = false;
        if (!result.ok() && !result.cancelled())
            m_error = format(tr("The self-test could not be started: {message}"), {{"message", result.errorMessage}});
        build();
    });
}

// What the dialog shows, compared before building it again: most changes
// on the bus are to other objects.
struct SmartRow
{
    std::string name;
    std::string title;
    std::string value;

    bool operator==(const SmartRow&) const = default;
};

struct SmartPage
{
    std::vector<SmartRow> overview;
    std::string detailsTitle;
    std::vector<SmartRow> details;
    std::string error;
    bool canTest = false;
    bool testEnabled = false;

    bool operator==(const SmartPage&) const = default;
};

static void addRow(std::vector<SmartRow>& rows, const std::string& name, const std::string& title,
                   const std::string& value)
{
    if (!value.empty())
        rows.push_back({name, title, value});
}

void SmartDialog::build()
{
    const udisks::Object* drive = m_context.model.object(m_drive);
    if (!drive)
        return;

    SmartPage page;
    std::vector<SmartRow>& overview = page.overview;
    const udisks::SmartStatus status = udisks::smartStatus(m_context.model, m_drive);
    addRow(overview, "smart:status", tr("Status"), udisks::smartStatusName(status));
    std::string selftest;
    bool testing = false;
    uint64_t updated = 0;
    if (drive->ata) {
        const udisks::Ata& ata = *drive->ata;
        addRow(overview, "smart:temperature", tr("Temperature"), udisks::formatTemperature(ata.smartTemperature));
        if (ata.smartPowerOnSeconds > 0)
            addRow(overview, "smart:powerOn", tr("Powered On"),
                   udisks::formatPowerOnHours(ata.smartPowerOnSeconds / 3600));
        if (ata.smartNumBadSectors >= 0)
            addRow(overview, "smart:badSectors", tr("Bad Sectors"),
                   udisks::formatCount(uint64_t(ata.smartNumBadSectors)));
        selftest = udisks::selftestStatusText(ata.smartSelftestStatus, ata.smartSelftestPercentRemaining);
        testing = ata.smartSelftestStatus == "inprogress";
        updated = ata.smartUpdated;
        page.canTest = udisks::canSelftest(*drive);
    } else if (drive->nvme) {
        const udisks::NvmeController& nvme = *drive->nvme;
        addRow(overview, "smart:temperature", tr("Temperature"), udisks::formatTemperature(nvme.smartTemperature));
        if (nvme.smartUpdated > 0)
            addRow(overview, "smart:powerOn", tr("Powered On"), udisks::formatPowerOnHours(nvme.smartPowerOnHours));
        std::string warnings;
        for (const std::string& warning : nvme.smartCriticalWarning)
            warnings += (warnings.empty() ? "" : ", ") + warning;
        addRow(overview, "smart:warnings", tr("Critical Warnings"), warnings);
        selftest = udisks::selftestStatusText(nvme.smartSelftestStatus, nvme.smartSelftestPercentRemaining);
        testing = nvme.smartSelftestStatus == "inprogress";
        updated = nvme.smartUpdated;
        page.canTest = udisks::canSelftest(*drive);
    }
    addRow(overview, "smart:selftest", tr("Self-Test"), selftest);
    if (updated > 0)
        addRow(overview, "smart:updated", tr("Last Read"), udisks::formatDate(int64_t(updated)));
    page.testEnabled = !testing && !m_starting;

    if (m_attributes && drive->ata) {
        page.detailsTitle = tr("Attributes");
        for (const udisks::AtaAttribute& attribute : udisks::parseAtaAttributes(*m_attributes)) {
            std::string value = udisks::attributeValue(attribute);
            if (value.empty() && attribute.value >= 0)
                value = std::to_string(attribute.value);
            if (attribute.failing())
                value = format(tr("Failing: {value}"), {{"value", value}});
            page.details.push_back({"smart:attribute:" + std::to_string(attribute.id),
                                    udisks::attributeTitle(attribute), value.empty() ? std::string("—") : value});
        }
    } else if (m_attributes && drive->nvme) {
        page.detailsTitle = tr("Health");
        size_t index = 0;
        for (const auto& [title, value] : udisks::describeNvmeHealth(*m_attributes))
            addRow(page.details, "smart:health:" + std::to_string(index++), title, value);
    }
    page.error = m_error;

    if (m_built && page == *m_built)
        return;
    m_built = std::make_shared<SmartPage>(page);

    auto* content = m_dialog->setContent<ScrollingForm>("smart:scroll");
    Form* form = content->form;
    Card* card = form->addCard();
    for (const SmartRow& row : page.overview)
        card->addChild<InfoRow>(row.title, row.value)->setName(row.name);
    if (!page.details.empty()) {
        Card* details = form->addSection(page.detailsTitle);
        for (const SmartRow& row : page.details)
            details->addChild<InfoRow>(row.title, row.value)->setName(row.name);
    }
    auto* note = form->add<Footnote>(page.error);
    note->setName("smart:error");
    note->setError(true);

    if (page.canTest) {
        auto* start = content->footer->addEnd<DialogButton>(tr("Run Self-Test"), true);
        start->setName("smart:selftest:start");
        start->setEnabled(page.testEnabled);
        start->onClicked = [this] { startSelftest(); };
    }
}

} // namespace dialogs
