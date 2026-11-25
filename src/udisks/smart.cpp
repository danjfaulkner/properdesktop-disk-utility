// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "udisks/smart.h"

#include "udisks/describe.h"
#include "udisks/model.h"
#include "udisks/properties.h"

#include "base/i18n.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using properdesktop::i18n::trn;

namespace udisks {

SmartStatus smartStatus(const Model& model, const std::string& drive)
{
    const Object* object = model.object(drive);
    if (!object)
        return SmartStatus::NotSupported;
    if (object->ata) {
        const Ata& ata = *object->ata;
        if (!ata.smartSupported || !ata.smartEnabled || ata.smartUpdated == 0)
            return SmartStatus::NotSupported;
        if (ata.smartFailing || ata.smartNumAttributesFailing > 0)
            return SmartStatus::Failing;
        return SmartStatus::Verified;
    }
    if (object->nvme) {
        const NvmeController& nvme = *object->nvme;
        if (nvme.smartUpdated == 0)
            return SmartStatus::NotSupported;
        return nvme.smartCriticalWarning.empty() ? SmartStatus::Verified : SmartStatus::Failing;
    }
    return SmartStatus::NotSupported;
}

bool canSelftest(const Object& drive)
{
    if (drive.ata)
        return drive.ata->smartSupported && drive.ata->smartEnabled && drive.ata->smartUpdated > 0
                && !drive.ata->smartSelftestStatus.empty();
    if (drive.nvme)
        return !drive.nvme->smartSelftestStatus.empty();
    return false;
}

std::string smartStatusName(SmartStatus status)
{
    switch (status) {
    case SmartStatus::NotSupported:
        return tr("Not Supported");
    case SmartStatus::Verified:
        return tr("Verified");
    case SmartStatus::Failing:
        return tr("Failing");
    }
    return {};
}

std::string formatTemperature(double kelvin)
{
    if (kelvin <= 0)
        return {};
    return format(tr("{n} °C"), {{"n", std::to_string(std::lround(kelvin - 273.15))}});
}

std::string formatPowerOnHours(uint64_t hours)
{
    const std::string inHours = trn("{n} hour", "{n} hours", hours);
    const std::string counted = format(inHours, {{"n", formatCount(hours)}});
    if (hours < 48)
        return counted;
    const std::string days = format(trn("{n} day", "{n} days", hours / 24), {{"n", formatCount(hours / 24)}});
    return format(tr("{days} ({hours})"), {{"days", days}, {"hours", counted}});
}

std::string selftestStatusText(const std::string& status, int32_t percentRemaining)
{
    if (status.empty())
        return {};
    if (status == "inprogress") {
        if (percentRemaining < 0)
            return tr("In progress");
        return format(tr("In progress, {n}% left"), {{"n", std::to_string(percentRemaining)}});
    }
    if (status == "success")
        return tr("Completed without error");
    if (status == "aborted" || status == "aborted_unknown" || status == "aborted_format"
        || status == "aborted_sanitize" || status == "ctrl_reset" || status == "ns_removed")
        return tr("Aborted");
    if (status == "interrupted")
        return tr("Interrupted by a reset");
    if (status == "error_electrical")
        return tr("Failed: electrical error");
    if (status == "error_servo")
        return tr("Failed: servo error");
    if (status == "error_read")
        return tr("Failed: read error");
    if (status == "error_handling")
        return tr("Failed: damage found");
    return tr("Failed");
}

std::vector<AtaAttribute> parseAtaAttributes(const Variant::Items& values)
{
    using Row = std::tuple<uint8_t, std::string, uint16_t, int32_t, int32_t, int32_t, int64_t, int32_t, VariantMap>;
    std::vector<AtaAttribute> attributes;
    if (values.empty())
        return attributes;
    const auto rows = values.front().get<std::vector<Row>>();
    if (!rows)
        return attributes;
    for (const Row& row : *rows) {
        AtaAttribute attribute;
        attribute.id = std::get<0>(row);
        attribute.name = std::get<1>(row);
        attribute.flags = std::get<2>(row);
        attribute.value = std::get<3>(row);
        attribute.worst = std::get<4>(row);
        attribute.threshold = std::get<5>(row);
        attribute.pretty = std::get<6>(row);
        attribute.prettyUnit = std::get<7>(row);
        attributes.push_back(std::move(attribute));
    }
    return attributes;
}

std::string attributeTitle(const AtaAttribute& attribute)
{
    if (attribute.name.empty())
        return format(tr("Attribute {n}"), {{"n", std::to_string(attribute.id)}});
    // The names are libatasmart's, in lower case joined by dashes.
    std::string title = attribute.name;
    bool wordStart = true;
    for (char& c : title) {
        if (c == '-' && &c != &title.front()) {
            wordStart = true;
            c = ' ';
            continue;
        }
        if (wordStart && c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
        wordStart = false;
    }
    // Power-on and the like keep their dash.
    for (const char* joined : {"Power On", "Spin Up", "Read Write", "Seek Error", "Start Stop", "G Sense",
                               "Load Unload", "Head Flying", "Power Off"}) {
        const std::string spaced = joined;
        const size_t at = title.find(spaced);
        if (at != std::string::npos)
            title[at + spaced.find(' ')] = '-';
    }
    return title;
}

std::string attributeValue(const AtaAttribute& attribute)
{
    const auto count = [](int64_t value) {
        return value < 0 ? "-" + formatCount(uint64_t(-value)) : formatCount(uint64_t(value));
    };
    switch (attribute.prettyUnit) {
    case 2: {
        const uint64_t hours = uint64_t(std::max<int64_t>(0, attribute.pretty)) / 3600000;
        if (hours > 0)
            return formatPowerOnHours(hours);
        return format(tr("{n} ms"), {{"n", count(attribute.pretty)}});
    }
    case 3:
        return format(trn("{n} sector", "{n} sectors", uint64_t(std::max<int64_t>(0, attribute.pretty))),
                      {{"n", count(attribute.pretty)}});
    case 4:
        return formatTemperature(double(attribute.pretty) / 1000);
    case 1:
        return count(attribute.pretty);
    default:
        return attribute.pretty == 0 ? std::string() : count(attribute.pretty);
    }
}

template<typename T>
static std::optional<uint64_t> unsignedValue(const VariantMap& map, const char* key)
{
    const auto it = map.find(key);
    if (it == map.end())
        return std::nullopt;
    if (const auto value = it->second.get<T>())
        return uint64_t(*value);
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> describeNvmeHealth(const Variant::Items& values)
{
    std::vector<std::pair<std::string, std::string>> rows;
    if (values.empty())
        return rows;
    const auto map = values.front().get<VariantMap>();
    if (!map)
        return rows;
    const auto percent = [](uint64_t value) { return std::to_string(value) + "%"; };
    const auto minutes = [](uint64_t value) {
        return format(trn("{n} minute", "{n} minutes", value), {{"n", formatCount(value)}});
    };
    if (const auto spare = unsignedValue<uint8_t>(*map, "avail_spare")) {
        std::string text = percent(*spare);
        if (const auto threshold = unsignedValue<uint8_t>(*map, "spare_thresh"))
            text = format(tr("{value} (threshold {threshold})"), {{"value", text}, {"threshold", percent(*threshold)}});
        rows.emplace_back(tr("Available Spare"), text);
    }
    if (const auto used = unsignedValue<uint8_t>(*map, "percent_used"))
        rows.emplace_back(tr("Wear"), percent(*used));
    if (const auto read = unsignedValue<uint64_t>(*map, "total_data_read"))
        rows.emplace_back(tr("Data Read"), formatSize(*read));
    if (const auto written = unsignedValue<uint64_t>(*map, "total_data_written"))
        rows.emplace_back(tr("Data Written"), formatSize(*written));
    if (const auto busy = unsignedValue<uint64_t>(*map, "ctrl_busy_time"))
        rows.emplace_back(tr("Busy Time"), minutes(*busy));
    if (const auto cycles = unsignedValue<uint64_t>(*map, "power_cycles"))
        rows.emplace_back(tr("Power Cycles"), formatCount(*cycles));
    if (const auto unsafe = unsignedValue<uint64_t>(*map, "unsafe_shutdowns"))
        rows.emplace_back(tr("Unsafe Shutdowns"), formatCount(*unsafe));
    if (const auto errors = unsignedValue<uint64_t>(*map, "media_errors"))
        rows.emplace_back(tr("Media Errors"), formatCount(*errors));
    if (const auto entries = unsignedValue<uint64_t>(*map, "num_err_log_entries"))
        rows.emplace_back(tr("Error Log Entries"), formatCount(*entries));
    if (const auto warning = unsignedValue<uint16_t>(*map, "wctemp"); warning && *warning > 0)
        rows.emplace_back(tr("Warning Temperature"), formatTemperature(double(*warning)));
    if (const auto critical = unsignedValue<uint16_t>(*map, "cctemp"); critical && *critical > 0)
        rows.emplace_back(tr("Critical Temperature"), formatTemperature(double(*critical)));
    if (const auto above = unsignedValue<uint32_t>(*map, "warning_temp_time"))
        rows.emplace_back(tr("Time Above Warning"), minutes(*above));
    if (const auto above = unsignedValue<uint32_t>(*map, "critical_temp_time"))
        rows.emplace_back(tr("Time Above Critical"), minutes(*above));
    return rows;
}

} // namespace udisks
