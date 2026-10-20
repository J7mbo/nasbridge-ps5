// NASBridge - Keeping the PS5 out of rest mode while a copy runs.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 goes into rest mode after a while without input (Settings > Power
// Saving). sceSystemServicePowerTick resets that idle timer; PS5Upload calls
// it every two minutes during uploads. NASBridge calls it while a copy or a
// delete runs. Putting the PS5 into rest mode by hand still works.

#pragma once

namespace hui::concepts
{

// Resets the PS5's idle timer. False when the console doesn't have the
// function (it is looked up, never called blindly) or refuses.
bool keep_awake_tick();

} // namespace hui::concepts
