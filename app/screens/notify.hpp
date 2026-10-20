// NASBridge - PS5 system notifications.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The toast in the PS5's corner, the one the installer shows too, so a copy
// that finishes or fails is noticed while you're playing something else.

#pragma once

#include <functional>
#include <string>

namespace hui::concepts
{

// Shows `text` as a PS5 notification. Safe from any thread. Does nothing
// when the console doesn't have the function (it is never called blindly).
void ps5_notify(const std::string &text);

// For tests: where notifications go instead (empty: the console).
void set_notify_hook(std::function<void(const std::string &)> hook);

} // namespace hui::concepts
