// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "views/style.h"

#include "ui/theme.h"

#include <gtest/gtest.h>

using namespace properdesktop::ui;
using namespace udisks::fake;

// The eject plate keeps one gap to the row's top, end and bottom, and its
// corners are the row's less that gap.
TEST(LayoutTest, EjectPlateIsInsetEvenly)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    const RectF row = driver.frame("sidebar:v:" + sample::kStickVolume);
    const RectF eject = driver.frame("sidebar:eject:v:" + sample::kStickVolume);
    const double top = eject.y - row.y;
    const double right = row.x + row.width - (eject.x + eject.width);
    const double bottom = row.y + row.height - (eject.y + eject.height);
    EXPECT_DOUBLE_EQ(top, views::metrics::ejectGap);
    EXPECT_DOUBLE_EQ(right, top);
    EXPECT_DOUBLE_EQ(bottom, top);
    EXPECT_GT(Theme::current().mediumRadius - views::metrics::ejectGap, 0);
}

// The summary card's icon, usage bar and the grid's text keep the card's
// padding on every side they meet.
TEST(LayoutTest, CardsKeepTheirPadding)
{
    Scenario scenario;
    TestDriver& driver = scenario.driver;
    driver.click("sidebar:v:" + sample::kRoot);
    const double padding = views::metrics::cardPadding;

    const RectF card = driver.frame("detail:summary");
    const RectF icon = driver.frame("detail:icon");
    EXPECT_DOUBLE_EQ(icon.x - card.x, padding);
    EXPECT_DOUBLE_EQ(icon.y - card.y, padding);
    const RectF bar = driver.frame("detail:usage");
    EXPECT_DOUBLE_EQ(bar.x - card.x, padding);
    EXPECT_DOUBLE_EQ(card.x + card.width - (bar.x + bar.width), padding);

    const RectF grid = driver.frame("detail:info");
    const RectF title = driver.frame("detail:title:mountPoint");
    EXPECT_DOUBLE_EQ(title.x - grid.x, padding);
    // The right column's last value ends one padding before the card's edge.
    const RectF device = driver.frame("detail:device");
    EXPECT_DOUBLE_EQ(grid.x + grid.width - (device.x + device.width), padding);
    // The two cards line up.
    EXPECT_DOUBLE_EQ(grid.x, card.x);
    EXPECT_DOUBLE_EQ(grid.width, card.width);
}
