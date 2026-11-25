// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "views/style.h"

#include "ui/bitmap.h"
#include "ui/scroll_bar.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <map>

using namespace properdesktop::ui;
using namespace udisks::fake;
using namespace std::chrono_literals;

// How far each end of a scroll view's bar must stay from its edges: the
// corner padding of what it meets, the header above the detail pane.
struct BarRule
{
    double top;
    double bottom;
    // The bar runs along the window's right edge.
    bool atWindowEdge = false;
};

static std::map<std::string, BarRule> rules()
{
    const Theme& theme = Theme::current();
    const double window = ScrollBar::cornerPadding(theme.windowRadius);
    return {
        // A side column: under the header, and its foot meets the detail
        // pane, not a corner of the window.
        {"sidebar:scroll", {ScrollBar::inset, ScrollBar::inset}},
        {"detail:scroll", {views::metrics::headerHeight + ScrollBar::inset, window}},
        // Edge to edge under the title: the bottom is the window's when
        // there is no footer, which the check below takes into account.
        {"info:scroll", {ScrollBar::inset, ScrollBar::inset, true}},
        {"smart:scroll", {ScrollBar::inset, ScrollBar::inset, true}},
    };
}

static void collect(View* view, std::vector<ScrollView*>& found)
{
    if (auto* scroll = dynamic_cast<ScrollView*>(view))
        found.push_back(scroll);
    for (const auto& child : view->children())
        collect(child.get(), found);
}

static ScrollBar* verticalBar(ScrollView& scroll)
{
    for (const auto& child : scroll.children()) {
        auto* bar = dynamic_cast<ScrollBar*>(child.get());
        if (bar && bar->geometry().height > bar->geometry().width)
            return bar;
    }
    return nullptr;
}

// The rows of the handle's ink in the bar, scrolled to the top and to the
// bottom of a long content: how far its ends stay from the bar's.
static int firstInkRow(const Bitmap& bitmap, int x)
{
    for (int y = 0; y < bitmap.height(); ++y) {
        if (bitmap.alpha(x, y) > 0)
            return y;
    }
    return -1;
}

static int lastInkRow(const Bitmap& bitmap, int x)
{
    for (int y = bitmap.height() - 1; y >= 0; --y) {
        if (bitmap.alpha(x, y) > 0)
            return y;
    }
    return -1;
}

static std::pair<int, int> handleGaps(TestDriver& driver, ScrollBar& bar)
{
    const double viewport = bar.geometry().height;
    const int x = int(ScrollBar::thickness / 2);
    // The bar fades in over a few frames; a layout may set the scroll
    // view's own range again, so the long one is set before every look.
    bar.setRange(viewport, viewport * 8, 0);
    bar.setAlwaysShown(true);
    int first = -1;
    for (int frame = 0; frame < 60 && first < 0; ++frame) {
        TestDriver::waitFor([] { return false; }, 20ms);
        driver.layout();
        bar.setRange(viewport, viewport * 8, 0);
        bar.setAlwaysShown(true);
        first = firstInkRow(bar.grab(1), x);
    }
    bar.setRange(viewport, viewport * 8, viewport * 7);
    const Bitmap bottom = bar.grab(1);
    const int last = lastInkRow(bottom, x);
    bar.setAlwaysShown(false);
    return {first, last < 0 ? -1 : bottom.height() - 1 - last};
}

// Every scroll view of the window, each with a rule, keeps its bar's ends
// clear by at least the rule's padding; the ink may start on the pixel row
// the padding ends in.
static void checkWindow(TestDriver& driver, const std::string& where)
{
    driver.layout();
    std::vector<ScrollView*> scrolls;
    collect(driver.window().contentView(), scrolls);
    const std::map<std::string, BarRule> table = rules();
    for (ScrollView* scroll : scrolls) {
        const auto rule = table.find(scroll->name());
        if (rule == table.end()) {
            ADD_FAILURE() << where << ": a scroll view without a bar rule, named \"" << scroll->name() << "\"";
            continue;
        }
        ScrollBar* bar = verticalBar(*scroll);
        ASSERT_NE(bar, nullptr) << where << ": " << scroll->name();
        const double windowWidth = driver.window().size().width;
        const double windowHeight = driver.window().size().height;
        const PointF barEnd = bar->mapToWindow({bar->geometry().width, bar->geometry().height});
        if (rule->second.atWindowEdge)
            EXPECT_DOUBLE_EQ(barEnd.x, windowWidth) << where << ": " << scroll->name() << " is off the window's edge";
        // A bar that reaches the window's bottom right keeps clear of its corner.
        double bottomRule = rule->second.bottom;
        if (std::abs(barEnd.y - windowHeight) < 0.5 && std::abs(barEnd.x - windowWidth) < 0.5)
            bottomRule = std::max(bottomRule, ScrollBar::cornerPadding(Theme::current().windowRadius));
        const auto [top, bottom] = handleGaps(driver, *bar);
        ASSERT_GE(top, 0) << where << ": " << scroll->name() << " drew no handle";
        ASSERT_GE(bottom, 0) << where << ": " << scroll->name() << " drew no handle";
        EXPECT_GE(top + 1, rule->second.top) << where << ": " << scroll->name() << "'s top end";
        EXPECT_GE(bottom + 1, bottomRule) << where << ": " << scroll->name() << "'s bottom end";
    }
}

static std::unique_ptr<TestDriver> over(TestDriver& driver)
{
    Window* window = driver.dialog();
    if (!window)
        return nullptr;
    auto dialog = std::make_unique<TestDriver>(*window, [](const std::string& message) { ADD_FAILURE() << message; });
    dialog->layout();
    return dialog;
}

class ScrollBarLayout : public testing::Test
{
protected:
    void select(const std::string& id)
    {
        scenario.window->select(id);
        scenario.driver.layout();
    }

    // Opens a dialog with the action on the node and checks it.
    void checkDialog(Action action, const std::string& id, udisks::ViewMode mode = udisks::ViewMode::Volumes)
    {
        scenario.window->setViewMode(mode);
        select(id);
        ASSERT_TRUE(scenario.window->actions().trigger(action)) << actionName(action);
        std::unique_ptr<TestDriver> dialog = over(scenario.driver);
        ASSERT_TRUE(dialog) << actionName(action) << " opened no dialog";
        checkWindow(*dialog, actionName(action));
    }

    Scenario scenario;
};

TEST_F(ScrollBarLayout, MainWindow)
{
    checkWindow(scenario.driver, "window");
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    checkWindow(scenario.driver, "window, all devices");
}

TEST_F(ScrollBarLayout, Erase)
{
    checkDialog(Action::Erase, "v:" + sample::kData);
}

TEST_F(ScrollBarLayout, Partition)
{
    checkDialog(Action::Partition, "d:" + sample::kSata, udisks::ViewMode::AllDevices);
}

TEST_F(ScrollBarLayout, AddPartition)
{
    checkDialog(Action::AddVolume, "d:" + sample::kSata, udisks::ViewMode::AllDevices);
}

TEST_F(ScrollBarLayout, Restore)
{
    checkDialog(Action::Restore, "v:" + sample::kData);
}

TEST_F(ScrollBarLayout, NewBlankImage)
{
    checkDialog(Action::NewBlankImage, "v:" + sample::kData);
}

TEST_F(ScrollBarLayout, Rename)
{
    checkDialog(Action::Rename, "v:" + sample::kData);
}

TEST_F(ScrollBarLayout, FirstAid)
{
    checkDialog(Action::FirstAid, "v:" + sample::kData);
}

TEST_F(ScrollBarLayout, InfoAndSmart)
{
    checkDialog(Action::Info, "v:" + sample::kData);

    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    select("d:" + sample::kSata);
    ASSERT_TRUE(scenario.window->actions().trigger(Action::Info));
    std::unique_ptr<TestDriver> info = over(scenario.driver);
    ASSERT_TRUE(info);
    checkWindow(*info, "info of a disk");
    info->click("info:smart");
    std::unique_ptr<TestDriver> smart = over(*info);
    ASSERT_TRUE(smart);
    checkWindow(*smart, "smart");
}
