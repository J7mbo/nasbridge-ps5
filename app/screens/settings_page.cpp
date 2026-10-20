// NASBridge - The Settings screen.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/settings_page.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <cmath>
#include <cstring>

namespace hui::concepts
{

namespace
{

using gfx::Rect;

constexpr float kMargin = 96.0f;
// Beside the title, as the library's filter tabs sit beside its own: about
// 50 px after the word, centred on its capitals (64 px type on baseline 128).
const Rect kTabs{414.0f, 75.0f, 820.0f, 60.0f};
// The same column the Connection page's fields use.
const Rect kForm{kMargin, 236.0f, 1004.0f, 680.0f};

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

const char *subtitle(int section)
{
    switch (section)
    {
    case 1:
        return "How NASBridge copies games from the NAS";
    case 2:
        return "What NASBridge sounds like";
    case 3:
        return "Extra information, for finding problems";
    default:
        return "";
    }
}

} // namespace

SettingsPage::SettingsPage(const ui::Fonts &fonts, ConnectionPage &connection)
    : fonts_(fonts), connection_(connection)
{
    settings_.load();
    settings_.apply();
    connection_.set_embedded(true);

    tabs_.style.theme = acrylic();
    tabs_.style.kind = ui::TabKind::pill;
    tabs_.style.on_page = true;
    tabs_.style.text_size = 22.0f;
    tabs_.style.height = 52.0f;
    tabs_.style.focus_ring = false; // the section holds the focus; L2 / R2 turn the tabs
    tabs_.set_tabs({{"Connection"}, {"Copying"}, {"Sound"}, {"Debug"}});
    tabs_.set_bounds(kTabs);
    tabs_.set_focused(false);
    tabs_.set_active(kConnection, true);
    build();
}

void SettingsPage::build()
{
    for (ui::Form *f : {&copying_, &sound_, &debug_})
    {
        f->clear();
        f->style.theme = acrylic();
        f->style.on_page = true;
        f->set_bounds(kForm);
    }
    // Both steppers are connections to the NAS, used two ways: a big file is
    // split between several, while small files come one per connection.
    copying_.add_header("Big files (16 MB and over)");
    copying_.add_stepper(kConnections, "Connections per file", settings_.connections, 1, 4)
        .description = "Each big file is split across this many. Fewer is gentler on a one-disk NAS.";
    copying_.add_header("Game folders of small files");
    copying_.add_stepper(kSmallFiles, "Files at once", settings_.small_files, 1, 16)
        .description = "Each small file gets its own connection. More copies a folder faster.";
    copying_.add_header("While copying");
    copying_.add_toggle(kKeepAwake, "Keep the PS5 awake", settings_.keep_awake)
        .description = "The PS5 stays out of rest mode while a copy or a delete runs.";
    sound_.add_toggle(kSounds, "Sounds", settings_.sounds).description =
        "Every sound NASBridge makes. Off is silent; the controller still rumbles.";
    debug_.add_toggle(kDebugInfo, "Show debug info", settings_.debug).description =
        "Adds diagnostic lines, such as the copy helper's connections in the queue panel.";
}

ui::Form &SettingsPage::form(Section section)
{
    return section == kCopying ? copying_ : section == kSound ? sound_ : debug_;
}

const ui::Form &SettingsPage::form(Section section) const
{
    return section == kCopying ? copying_ : section == kSound ? sound_ : debug_;
}

void SettingsPage::enter()
{
    connection_.enter();
    for (ui::Form *f : {&copying_, &sound_, &debug_})
    {
        f->set_focus(0, true);
        f->enter();
    }
}

void SettingsPage::store()
{
    settings_.connections = copying_.stepper_value(kConnections);
    settings_.small_files = copying_.stepper_value(kSmallFiles);
    settings_.keep_awake = copying_.toggle_value(kKeepAwake);
    settings_.sounds = sound_.toggle_value(kSounds);
    settings_.debug = debug_.toggle_value(kDebugInfo);
    settings_.apply();
    save_error_ = settings_.save();
}

bool SettingsPage::handle(const InputFrame &input, app::Feedback &feedback)
{
    const Section section = static_cast<Section>(tabs_.active());
    // A popup on the Connection page (keyboard, list, folder picker) takes
    // everything, the tab buttons too.
    if (section == kConnection && connection_.modal_open())
        return connection_.handle(input, feedback);

    const int turn = input.is_pressed(Action::jump_next)   ? 1
                     : input.is_pressed(Action::jump_prev) ? -1
                                                           : 0;
    if (turn != 0)
    {
        if (tabs_.step(turn, input, feedback) == ui::Event::changed &&
            tabs_.active() == kConnection)
            connection_.enter();
        return true;
    }

    if (section == kConnection)
        return connection_.handle(input, feedback);

    if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        return false;
    ui::Form &f = form(section);
    if (f.handle(input, feedback) == ui::Event::changed)
        store();
    return true;
}

void SettingsPage::update(float dt)
{
    const int section = tabs_.active();
    tabs_.update(dt);
    copying_.set_active(section == kCopying);
    sound_.set_active(section == kSound);
    debug_.set_active(section == kDebug);
    copying_.update(dt);
    sound_.update(dt);
    debug_.update(dt);
}

void SettingsPage::draw_section(gfx::DrawList &list, std::uint32_t glass, float clock,
                                Section section, float shown) const
{
    const ui::Theme &theme = acrylic();
    ui::Painter paint(list, fonts_, theme, glass);
    paint.body(subtitle(section), kMargin, 180, 24, paint.page_text_muted());
    ui::Canvas canvas{list, fonts_, glass, clock};
    form(section).draw(canvas);
    if (!save_error_.empty())
        paint.body(ui::fit_label(paint, "Couldn't save: " + save_error_, 22.0f, 1000.0f), kMargin,
                   950, 22, theme.warning);
    (void)shown;
    const ui::Hint hints[] = {{ui::Button::dpad, "Move"},
                              {ui::Button::cross, "Change"},
                              {ui::Button::l2, "Section", ui::Button::r2},
                              {ui::Button::circle, "Back"}};
    ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), hints, 4, 1824, true);
}

void SettingsPage::draw(gfx::DrawList &list, std::uint32_t glass, float clock, float t) const
{
    // The title and tabs arrive with the content, as the Connection page's do.
    const float content = tween::smoothstep(tween::clamp01((t - 0.5f) / 0.5f));
    if (content > 0.001f)
    {
        const ui::Theme &theme = acrylic();
        ui::Painter paint(list, fonts_, theme, glass);
        list.push_opacity(content);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 28.0f * (1.0f - content));
        paint.heading("Settings", kMargin - 2, 128, 64, paint.page_text());
        ui::Canvas canvas{list, fonts_, glass, clock};
        tabs_.draw(canvas);
        list.pop_transform();
        list.pop_opacity();
    }

    // Each section fades by its distance from the gliding tab, so turning
    // the tabs cross-fades the two sections it passes between.
    const float at = tabs_.active_value();
    for (int i = 0; i < kSections; ++i)
    {
        const float shown = 1.0f - tween::clamp01(std::fabs(at - static_cast<float>(i)) * 1.6f);
        if (shown <= 0.001f)
            continue;
        if (i == kConnection)
        {
            list.push_opacity(shown);
            connection_.draw(list, glass, clock, t);
            list.pop_opacity();
            continue;
        }
        if (content <= 0.001f)
            continue;
        list.push_opacity(content * shown);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 28.0f * (1.0f - content));
        draw_section(list, glass, clock, static_cast<Section>(i), shown);
        list.pop_transform();
        list.pop_opacity();
    }
}

} // namespace hui::concepts
