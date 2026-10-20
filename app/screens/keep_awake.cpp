// NASBridge - Keeping the PS5 out of rest mode while a copy runs.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/keep_awake.hpp"

#if defined(__PROSPERO__)
extern "C" {
// libSceSystemService (already loaded by the app). Weak: a function the
// console's library doesn't have is left at address 0, and calling that
// crashes the app (seen on 13.60 with a ShellCoreUtil function).
__attribute__((weak)) int sceSystemServicePowerTick(void);
}
#endif

namespace hui::concepts
{

bool keep_awake_tick()
{
#if defined(__PROSPERO__)
    int (*volatile tick)(void) = &sceSystemServicePowerTick;
    return tick != nullptr && tick() == 0;
#else
    return true; // the PC preview has nothing to keep awake
#endif
}

} // namespace hui::concepts
