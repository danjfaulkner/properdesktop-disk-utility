// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "views/context.h"

#include "application.h"

namespace views {

Context::Context(Application& application, properdesktop::ui::Window& window, ActionDispatcher& actions)
    : application(application)
    , model(application.model())
    , operations(application.operations())
    , actions(actions)
    , window(window)
{
}

} // namespace views
