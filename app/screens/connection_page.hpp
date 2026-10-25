// NASBridge - The Connection screen (Options from the library).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the NAS is and how to sign in: connection type (a dropdown: WebDAV,
// FTP or SMB), server address, port, folder, user name and password. Each text
// opens the kit's on-screen keyboard (InputPrompt); the folder can also be
// picked by browsing the NAS (the button beside it, folder_picker.cpp).
// Every change is saved at once. "Test connection" asks the NAS for the folder's contents in the
// background (nas_probe.cpp). The right-hand panel shows the address
// NASBridge will use and the test's result.

#pragma once

#include "concepts/connection.hpp"
#include "concepts/folder_picker.hpp"
#include "concepts/nas_probe.hpp"

#include "app/concept.hpp"
#include "core/tween.hpp"
#include "ui/components.hpp"

namespace hui::concepts
{

class ConnectionPage
{
  public:
    explicit ConnectionPage(const ui::Fonts &fonts);

    // Back on the screen: the focus returns to the connection type.
    void enter();
    // False once the player has left (Circle or Options with nothing open).
    bool handle(const InputFrame &input, app::Feedback &feedback);
    void update(float dt);
    // t: 0 hidden .. 1 fully shown.
    void draw(gfx::DrawList &list, std::uint32_t glass, float clock, float t) const;

    const Connection &connection() const
    {
        return connection_;
    }
    // The latest "Test connection" result (idle when untested or changed since).
    ProbeResult test_result() const;
    // Runs the test without sounds (at launch, and after the settings change).
    void test_quietly();
    // While a copy needs these settings (running, or paused waiting to
    // resume from this NAS) they can't be changed: `reason` says why, and
    // empty unlocks them. Test connection still works.
    void set_locked(const std::string &reason)
    {
        locked_ = reason;
    }
    bool locked() const
    {
        return !locked_.empty();
    }
    // Inside the Settings screen: it draws the title and the tabs, so this
    // page leaves out its own title and adds the hint for changing tabs.
    void set_embedded(bool embedded)
    {
        embedded_ = embedded;
    }
    // A popup of this page is open (the keyboard, a list, the folder
    // picker): it takes every input, the tab buttons included.
    bool modal_open() const
    {
        return prompt_.is_open() || type_.is_open() || picker_.is_open();
    }
    // Goes up by one every time the settings are saved.
    int revision() const
    {
        return revision_;
    }

  private:
    enum Item
    {
        kType,
        kHost,
        kPort,
        kFolder,
        kUser,
        kPassword,
        kTest,   // the Test connection button
        kBrowse, // the folder button beside Folder
        kItems,
    };

    void layout();
    void sync_fields();
    void move(Direction direction, const InputFrame &input, app::Feedback &feedback);
    void edit(Item item, app::Feedback &feedback);
    void accept(app::Feedback &feedback);
    void store();
    void test(app::Feedback &feedback);
    void browse(app::Feedback &feedback);
    ui::TextField &field(Item item);
    const ui::TextField &field(Item item) const;
    void draw_panel(gfx::DrawList &list, ui::Painter &paint, std::uint32_t glass,
                    float clock) const;

    const ui::Fonts &fonts_;
    Connection connection_;
    ui::Select type_;
    ui::TextField fields_[kItems]; // only kHost .. kPassword are used
    gfx::Rect test_button_{};
    tween::Spring test_focus_;
    gfx::Rect browse_button_{};
    tween::Spring browse_focus_;
    FolderPicker picker_;
    ui::Pulse press_;
    NasProbe probe_;
    ProbeResult local_result_; // a refusal decided here (no address yet)
    ProbeResult::State heard_ = ProbeResult::State::idle; // for the result's sound
    int revision_ = 0;
    ui::InputPrompt prompt_;
    Item editing_ = kHost;
    int focus_ = kType;
    int column_ = 0; // the column up and down try to keep
    std::string save_error_;
    std::string locked_; // why the settings can't change now ("" when they can)
    ui::Pulse refusal_;
    float age_ = 0.0f;
    bool embedded_ = false;
};

} // namespace hui::concepts
