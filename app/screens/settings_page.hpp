// NASBridge - The Settings screen (Options from the library).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Four sections, one at a time, chosen with L2 / R2 on a row of tabs beside
// the title (as the library's filters are):
//   - Connection: where the NAS is and how to sign in (connection_page.cpp),
//   - Copying: connections per big file, small files at once (each its own
//     connection), and
//     whether a copy keeps the PS5 awake,
//   - Sound: whether NASBridge makes any sounds,
//   - Debug: whether diagnostics show (the copy helper's line in the queue
//     panel, and whatever is added there later).
// Every change is saved and applied at once (app_settings.cpp).

#pragma once

#include "concepts/app_settings.hpp"
#include "concepts/connection_page.hpp"

#include "app/concept.hpp"
#include "ui/components/form.hpp"
#include "ui/components/tabs.hpp"

#include <string>

namespace hui::concepts
{

class SettingsPage
{
  public:
    // The Connection page stays the library's (it reads the connection from
    // it); this screen shows it as its first section.
    SettingsPage(const ui::Fonts &fonts, ConnectionPage &connection);

    // Opening the screen: the section it was left on, focus at its top.
    void enter();
    // False once the player has left (Circle or Options with nothing open).
    bool handle(const InputFrame &input, app::Feedback &feedback);
    void update(float dt);
    // t: 0 hidden .. 1 fully shown.
    void draw(gfx::DrawList &list, std::uint32_t glass, float clock, float t) const;

    const AppSettings &settings() const
    {
        return settings_;
    }

  private:
    enum Section
    {
        kConnection,
        kCopying,
        kSound,
        kDebug,
        kSections,
    };
    enum Row
    {
        kConnections = 1,
        kSmallFiles,
        kKeepAwake,
        kSounds,
        kDebugInfo,
    };

    void build();
    void store();
    ui::Form &form(Section section);
    const ui::Form &form(Section section) const;
    void draw_section(gfx::DrawList &list, std::uint32_t glass, float clock, Section section,
                      float shown) const;

    const ui::Fonts &fonts_;
    ConnectionPage &connection_;
    ui::TabBar tabs_;
    ui::Form copying_;
    ui::Form sound_;
    ui::Form debug_;
    AppSettings settings_;
    std::string save_error_;
};

} // namespace hui::concepts
