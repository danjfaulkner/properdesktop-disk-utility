// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "scenario.h"

#include "udisks/backend.h"

#include "ui/scroll_bar.h"
#include "ui/scroll_view.h"
#include "ui/test_input.h"

#include <gtest/gtest.h>

#include <string>
#include <tuple>
#include <vector>

using namespace properdesktop::ui;
using namespace udisks::fake;
using properdesktop::base::dbus::Variant;
using properdesktop::base::dbus::VariantMap;

static std::unique_ptr<TestDriver> over(TestDriver& driver)
{
    Window* window = driver.dialog();
    if (!window)
        return nullptr;
    auto dialog = std::make_unique<TestDriver>(*window, [](const std::string& message) { ADD_FAILURE() << message; });
    dialog->layout();
    return dialog;
}

// The scroll view's bar runs along the dialog's right edge, and the pointer
// works it there: a drag of its handle and a wheel over the edge's strip
// both scroll.
static void checkBarAtTheEdge(TestDriver& driver, const std::string& name)
{
    driver.layout();
    Window& window = driver.window();
    auto* scroll = driver.find<ScrollView>(name);
    ASSERT_NE(scroll, nullptr);
    ASSERT_GT(scroll->contentSize().height, scroll->geometry().height) << name << " has nothing to scroll";
    const RectF frame = driver.frame(name);
    const double edge = window.size().width - ScrollBar::thickness / 2;
    ASSERT_DOUBLE_EQ(frame.x + frame.width, window.size().width) << name;

    // The handle starts one inset under the bar's top while at the top.
    const PointF grab{edge, frame.y + ScrollBar::inset + 2};
    TestInput::press(window, grab);
    TestInput::movePointer(window, {grab.x, grab.y + 60});
    TestInput::release(window, {grab.x, grab.y + 60});
    driver.layout();
    const double dragged = scroll->scrollPosition().y;
    EXPECT_GT(dragged, 0) << name << ": the handle's drag at the edge did not scroll";

    ScrollEvent wheel;
    wheel.value120Y = 120;
    wheel.deltaY = 10;
    for (const double x : {window.size().width - 1, window.size().width - 15}) {
        const double before = scroll->scrollPosition().y;
        TestInput::scroll(window, {x, frame.y + frame.height / 2}, wheel);
        driver.layout();
        EXPECT_GT(scroll->scrollPosition().y, before) << name << ": the wheel at x " << x << " did not scroll";
    }
}

TEST(DialogEdge, InfoOfAVolume)
{
    Scenario scenario;
    scenario.window->select("v:" + sample::kData);
    scenario.driver.layout();
    ASSERT_TRUE(scenario.window->actions().trigger(Action::Info));
    std::unique_ptr<TestDriver> info = over(scenario.driver);
    ASSERT_TRUE(info);
    checkBarAtTheEdge(*info, "info:scroll");
}

TEST(DialogEdge, InfoAndSmartOfADisk)
{
    Scenario scenario;
    scenario.window->setViewMode(udisks::ViewMode::AllDevices);
    scenario.window->select("d:" + sample::kSata);
    scenario.driver.layout();
    ASSERT_TRUE(scenario.window->actions().trigger(Action::Info));
    std::unique_ptr<TestDriver> info = over(scenario.driver);
    ASSERT_TRUE(info);
    checkBarAtTheEdge(*info, "info:scroll");

    ASSERT_TRUE(info->click("info:smart"));
    std::unique_ptr<TestDriver> smart = over(*info);
    ASSERT_TRUE(smart);
    // Enough attributes to scroll.
    using Row = std::tuple<uint8_t, std::string, uint16_t, int32_t, int32_t, int32_t, int64_t, int32_t, VariantMap>;
    std::vector<Row> rows;
    for (uint8_t id = 1; id <= 30; ++id)
        rows.push_back({id, "attribute-" + std::to_string(id), 0x33, 100, 100, 10, 0, 0, {}});
    auto& backend = static_cast<FakeBackend&>(scenario.application.backend());
    ASSERT_EQ(backend.pendingCount(), 1u);
    ASSERT_EQ(backend.pending(0).method, "SmartGetAttributes");
    backend.reply(0, {{}, {}, {Variant(rows)}});
    runPosted();
    checkBarAtTheEdge(*smart, "smart:scroll");
}
