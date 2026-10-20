// NASBridge - Checks that the file functions copying needs exist.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A function the console's system library doesn't have is left at address 0
// by the loader, and calling it crashes NASBridge (seen on 13.60 with a
// ShellCoreUtil function). Copying, deleting and the drive check use a few
// file functions nothing else in NASBridge does, so they are checked first.

#pragma once

#include <string>

namespace hui::concepts
{

// True when every one is there; otherwise false and *missing names them.
bool file_calls_available(std::string *missing);

} // namespace hui::concepts
