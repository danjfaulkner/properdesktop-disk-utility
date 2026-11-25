// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "views/context.h"

#include "ui/icon.h"
#include "ui/label.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace properdesktop::ui;
using namespace udisks::fake;

namespace {

class SidebarCentring : public testing::TestWithParam<double>
{
};

// Letters that reach below the baseline pull the ink's middle down; such a
// label is centred by its capitals alone.
bool hasDescender(const std::string& text)
{
    return text.find_first_of("gjpqy,;()") != std::string::npos;
}

} // namespace

// Each row's text is centred on its glyph: the capitals' middle, and the
// ink's middle for text without descenders, within half a device pixel of
// the glyph's drawn middle, with the glyph and the baseline on whole device
// pixels.
TEST_P(SidebarCentring, TextIsCentredOnTheGlyph)
{
    const double scale = GetParam();
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.window().setScale(scale);
    driver.layout();

    size_t measured = 0;
    for (const udisks::SectionNodes& section : scenario.window->context().tree) {
        for (const udisks::Node& node : section.nodes) {
            const std::string glyphName = "sidebar:glyph:" + node.id;
            const std::string labelName = "sidebar:label:" + node.id;
            auto* glyph = driver.find<Icon>(glyphName);
            auto* label = driver.find<Label>(labelName);
            ASSERT_NE(glyph, nullptr) << node.id;
            ASSERT_NE(label, nullptr) << node.id;
            const RectF glyphFrame = driver.frame(glyphName);
            const RectF labelFrame = driver.frame(labelName);

            const double glyphTop = glyphFrame.y * scale;
            EXPECT_NEAR(glyphTop, std::round(glyphTop), 1e-6) << node.id;
            const double glyphMiddle = (glyphFrame.y + glyphFrame.height / 2) * scale;
            const double baseline = (labelFrame.y + label->baselineOffset()) * scale;
            EXPECT_NEAR(baseline, std::round(baseline), 1e-6) << node.id;

            const double capMiddle = baseline - label->capHeight() * scale / 2;
            const RectF ink = label->inkBoundingRect();
            const double inkMiddle = baseline + (ink.y + ink.height / 2) * scale;
            std::printf("scale %.2f %-28s cap %+.3f ink %+.3f device px\n", scale, label->text().c_str(),
                        capMiddle - glyphMiddle, inkMiddle - glyphMiddle);
            EXPECT_LE(std::abs(capMiddle - glyphMiddle), 0.5) << node.id;
            if (!hasDescender(label->text()))
                EXPECT_LE(std::abs(inkMiddle - glyphMiddle), 0.5) << label->text();
            ++measured;
        }
    }
    EXPECT_GT(measured, 5u);
}

INSTANTIATE_TEST_SUITE_P(Scales, SidebarCentring, testing::Values(1.0, 1.75, 2.0));
