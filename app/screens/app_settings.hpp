// NASBridge - The app's own settings (the Settings screen's Copying, Sound and Debug tabs).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saved beside the connection in the app's save area (/download0/nasbridge),
// as key=value lines in the kit's checked save container. apply() hands them
// to the parts of NASBridge that use them.

#pragma once

#include <string>

namespace hui::concepts
{

struct AppSettings
{
    // Connections to the NAS: a big file is shared between this many, and
    // this many small files come in at once (1..4).
    int connections = 2;
    // Small files fetched at once, for folder games (1..16).
    int small_files = 16;
    // Folder games are copied into one .exfat image each (exfat_image.hpp)
    // rather than as folders of files.
    bool as_image = true;
    // The PS5 stays out of rest mode while a copy or a delete runs.
    bool keep_awake = true;
    // Every sound NASBridge makes (off: silent; the controller still rumbles).
    bool sounds = true;
    // Diagnostics: the copy helper's line in the queue panel, and whatever
    // in-app debugging is added later.
    bool debug = false;

    // False when there's nothing saved yet (the defaults stay).
    bool load();
    // "" when saved, or why not.
    std::string save() const;
    // Makes them take effect.
    void apply() const;
};

// Whether diagnostics are shown (AppSettings::debug, as last applied).
bool debug_info();
// Whether NASBridge makes sounds (AppSettings::sounds, as last applied).
bool sounds_on();
// Whether a copy or delete keeps the PS5 awake (AppSettings::keep_awake).
bool keep_awake_on();

} // namespace hui::concepts
