// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "images/progress.h"

#include "udisks/describe.h"

#include "base/i18n.h"

#include <algorithm>
#include <cmath>

using properdesktop::i18n::format;
using properdesktop::i18n::tr;
using properdesktop::i18n::trn;

namespace images {

static constexpr std::chrono::seconds kWindow{5};

void RateMeter::start(Clock::time_point now)
{
    m_samples.clear();
    m_samples.emplace_back(now, 0);
}

void RateMeter::add(Clock::time_point now, uint64_t copied)
{
    if (m_samples.empty()) {
        m_samples.emplace_back(now, copied);
        return;
    }
    m_samples.emplace_back(now, copied);
    // The oldest sample left still reaches back the whole window.
    while (m_samples.size() > 2 && now - m_samples[1].first >= kWindow)
        m_samples.pop_front();
}

double RateMeter::rate() const
{
    if (m_samples.size() < 2)
        return 0;
    const auto& [firstTime, firstBytes] = m_samples.front();
    const auto& [lastTime, lastBytes] = m_samples.back();
    const double seconds = std::chrono::duration<double>(lastTime - firstTime).count();
    if (seconds < 1 || lastBytes < firstBytes)
        return 0;
    return double(lastBytes - firstBytes) / seconds;
}

std::optional<std::chrono::seconds> RateMeter::remaining(uint64_t total) const
{
    const double bytesPerSecond = rate();
    if (bytesPerSecond <= 0)
        return std::nullopt;
    const uint64_t copied = m_samples.back().second;
    const double left = copied >= total ? 0 : double(total - copied) / bytesPerSecond;
    return std::chrono::seconds(int64_t(std::ceil(left)));
}

std::string amountText(uint64_t copied, uint64_t total)
{
    return format(tr("{copied} of {total}"),
                  {{"copied", udisks::formatSize(copied)}, {"total", udisks::formatSize(total)}});
}

std::string rateText(double bytesPerSecond)
{
    if (bytesPerSecond <= 0)
        return {};
    return format(tr("{size}/s"), {{"size", udisks::formatSize(uint64_t(bytesPerSecond))}});
}

std::string timeLeftText(std::chrono::seconds remaining)
{
    const int64_t seconds = remaining.count();
    if (seconds < 60) {
        // Counted in fives, so the text does not flicker every second.
        const int64_t shown = std::max<int64_t>(5, (seconds + 4) / 5 * 5);
        return trn("About {n} second left", "About {n} seconds left", uint64_t(shown));
    }
    if (seconds < 90 * 60) {
        const int64_t minutes = (seconds + 30) / 60;
        return trn("About {n} minute left", "About {n} minutes left", uint64_t(minutes));
    }
    const int64_t hours = (seconds + 1800) / 3600;
    return trn("About {n} hour left", "About {n} hours left", uint64_t(hours));
}

std::string progressText(uint64_t copied, uint64_t total, double bytesPerSecond)
{
    const std::string amount = amountText(copied, total);
    const std::string rate = rateText(bytesPerSecond);
    if (rate.empty())
        return amount;
    return format(tr("{amount} ({rate})"), {{"amount", amount}, {"rate", rate}});
}

} // namespace images
