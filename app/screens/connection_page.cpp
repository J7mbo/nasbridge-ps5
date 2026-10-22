// NASBridge - The Connection screen.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/connection_page.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

constexpr float kMargin = 96.0f;
constexpr float kColumnTop = 236.0f;
constexpr float kColumnWidth = 1004.0f; // 96 .. 1100
constexpr float kGap = 24.0f;           // between two fields on a row
constexpr float kRowGap = 22.0f;        // between rows
const Rect kPanel{1236.0f, 236.0f, 588.0f, 0.0f}; // height follows its content

const Color kWhite = Color::rgb(0xffffff);

// The rows of the screen, top to bottom; -1 is an empty second column.
constexpr int kRows[][2] = {{0, -1}, {1, 2}, {3, 7}, {4, 5}, {6, -1}};
constexpr float kBrowseWidth = 84.0f; // the folder button beside Folder
constexpr int kRowCount = static_cast<int>(std::size(kRows));

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

int row_of(int item)
{
    for (int r = 0; r < kRowCount; ++r)
        if (kRows[r][0] == item || kRows[r][1] == item)
            return r;
    return 0;
}

int columns_in(int row)
{
    return kRows[row][1] < 0 ? 1 : 2;
}

// Splits text into lines no wider than width.
std::vector<std::string> wrap(const ui::Painter &paint, std::string_view text, float size,
                              float width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find(' ', at);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string word(text.substr(at, end - at));
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && paint.body_width(tried, size) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = tried;
        }
        at = end + 1;
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

ConnectionPage::ConnectionPage(const ui::Fonts &fonts) : fonts_(fonts), picker_(fonts)
{
    const ui::Theme &theme = acrylic();
    connection_.load();

    type_.style.theme = theme;
    type_.style.on_page = true;
    type_.set_label("Connection type");
    // In ConnectionType's order: the list's index is the type.
    type_.set_options({{"WebDAV", "Built into most NAS systems"},
                       {"FTP", "Coming later"},
                       {"SMB", "Also built into most NAS systems"},
                       {"NFS", "Built into most NAS systems, no sign-in"}});
    std::vector<ui::SelectOption> options = type_.options();
    options[1].disabled = true;
    type_.set_options(std::move(options));
    type_.set_index(static_cast<int>(connection_.type));

    struct Setup
    {
        Item item;
        const char *label;
        const char *placeholder;
        const char *helper;
    };
    static constexpr Setup kSetup[] = {
        {kHost, "Server address", "Not set", "The NAS's IP address, like 192.168.1.20"},
        {kPort, "Port", "80", "Empty for 80"},
        {kFolder, "Folder", "/", "Where your games are, like /games, or browse for the folder"},
        {kUser, "User name", "None", "Leave empty if your NAS doesn't ask"},
        {kPassword, "Password", "None", "Kept only on this PS5"},
    };
    for (const Setup &setup : kSetup)
    {
        ui::TextField &f = field(setup.item);
        f.style.theme = theme;
        f.style.on_page = true;
        f.style.counter = false;
        f.style.password = setup.item == kPassword;
        f.set_label(setup.label);
        f.set_placeholder(setup.placeholder);
        f.set_helper(setup.helper);
    }

    prompt_.style.theme = theme;
    prompt_.style.allow_empty = true; // empty clears a value
    prompt_.style.auto_capital = false;
    // The keyboard's own Done key, Options as Done and Circle as Cancel, as
    // on the system keyboard: the kit's Cancel / Done row under the keys
    // would only repeat them.
    prompt_.style.buttons = false;
    prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
    prompt_.keyboard.style.bindings.done = Action::menu;
    // Typing and deleting make the same sound as Done (the kit's own click
    // per character was too much), at one pitch and from the centre, as
    // Done is: no rising pitch across the rows, no panning by key.
    prompt_.keyboard.style.type = audio::Cue::select;
    prompt_.keyboard.style.erase = audio::Cue::select;
    prompt_.keyboard.style.pitch_by_column = false;
    prompt_.style.sounds.pan = false; // the prompt hands its sounds to the keys

    sync_fields();
    layout();
}

ui::TextField &ConnectionPage::field(Item item)
{
    return fields_[item];
}

const ui::TextField &ConnectionPage::field(Item item) const
{
    return fields_[item];
}

void ConnectionPage::layout()
{
    const float select_h = type_.preferred_height();
    const float field_h = field(kHost).preferred_height();
    float y = kColumnTop;
    type_.set_bounds({kMargin, y, 520.0f, select_h});
    y += select_h + kRowGap + 8.0f;
    const float port_w = 220.0f;
    field(kHost).set_bounds({kMargin, y, kColumnWidth - port_w - kGap, field_h});
    field(kPort).set_bounds({kMargin + kColumnWidth - port_w, y, port_w, field_h});
    y += field_h + kRowGap;
    field(kFolder).set_bounds({kMargin, y, kColumnWidth - kBrowseWidth - kGap, field_h});
    {
        // Square, level with the field's box (not its label above).
        const gfx::Rect box = field(kFolder).field_rect();
        browse_button_ = {kMargin + kColumnWidth - kBrowseWidth, box.y, kBrowseWidth, box.h};
    }
    y += field_h + kRowGap;
    const float half = (kColumnWidth - kGap) * 0.5f;
    field(kUser).set_bounds({kMargin, y, half, field_h});
    field(kPassword).set_bounds({kMargin + half + kGap, y, half, field_h});
    y += field_h + kRowGap;
    test_button_ = {kMargin, y + 6.0f, 340.0f, 64.0f};
}

void ConnectionPage::sync_fields()
{
    field(kHost).set_text(connection_.host);
    field(kPort).set_text(connection_.port > 0 ? std::to_string(connection_.port) : "");
    field(kFolder).set_text(connection_.folder == "/" ? "" : connection_.folder);
    field(kUser).set_text(connection_.username);
    field(kPassword).set_text(connection_.password);
    // NFS has no sign-in: the fields stay, unused, so the screen keeps its shape.
    const bool nfs = connection_.type == ConnectionType::nfs;
    field(kUser).set_helper(nfs ? "Not used: NFS has no sign-in" : "Leave empty if your NAS doesn't ask");
    field(kPassword).set_helper(nfs ? "Not used: NFS has no sign-in" : "Kept only on this PS5");
    char port[16];
    std::snprintf(port, sizeof(port), "%d", connection_.default_port());
    field(kPort).set_placeholder(port);
    field(kPort).set_helper(std::string("Empty for ") + port);
}

void ConnectionPage::enter()
{
    focus_ = kType;
    column_ = 0;
    age_ = 0.0f;
    type_.dismiss();
    prompt_.dismiss();
}

ProbeResult ConnectionPage::test_result() const
{
    return local_result_.state != ProbeResult::State::idle ? local_result_ : probe_.result();
}

bool ConnectionPage::handle(const InputFrame &input, app::Feedback &feedback)
{
    // The test's answer arrives on its own thread. It's shown in the panel,
    // silently: the test makes no sounds of its own.
    heard_ = test_result().state;
    if (picker_.is_open())
    {
        if (picker_.handle(input, feedback) == FolderPicker::Result::chosen)
        {
            std::string folder = picker_.chosen();
            normalise_folder(&folder);
            connection_.folder = folder;
            store();
        }
        return true;
    }
    if (prompt_.is_open())
    {
        if (prompt_.handle(input, feedback) == ui::Event::activated)
            accept(feedback);
        return true;
    }
    if (type_.is_open())
    {
        if (type_.handle(input, feedback) == ui::Event::changed)
        {
            connection_.type = static_cast<ConnectionType>(type_.index());
            store();
        }
        return true;
    }
    if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        return false;
    if (input.nav != Direction::none)
        move(input.nav, input, feedback);
    if (input.is_pressed(Action::confirm))
    {
        if ((locked() && focus_ != kTest) || unused(focus_))
        {
            // A copy needs these settings as they are, or NFS doesn't use
            // this one: say so, change nothing.
            feedback.play(audio::Cue::error);
            refusal_.trigger();
            return true;
        }
        if (focus_ == kType)
            type_.handle(input, feedback); // opens the list
        else if (focus_ == kTest)
            test(feedback);
        else if (focus_ == kBrowse)
            browse(feedback);
        else
            edit(static_cast<Item>(focus_), feedback);
    }
    return true;
}

void ConnectionPage::move(Direction direction, const InputFrame &input, app::Feedback &feedback)
{
    const int row = row_of(focus_);
    const int column = kRows[row][1] == focus_ ? 1 : 0;
    int next = -1;
    if (direction == Direction::up || direction == Direction::down)
    {
        const int r = row + (direction == Direction::down ? 1 : -1);
        if (r >= 0 && r < kRowCount)
            next = kRows[r][std::min(column_, columns_in(r) - 1)];
    }
    else
    {
        const int c = column + (direction == Direction::right ? 1 : -1);
        if (c >= 0 && c < columns_in(row))
        {
            next = kRows[row][c];
            column_ = c;
        }
    }
    if (next < 0)
    {
        if (!input.nav_repeat)
        {
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
            refusal_.trigger();
        }
        return;
    }
    focus_ = next;
    feedback.play(audio::Cue::focus);
}

void ConnectionPage::edit(Item item, app::Feedback &feedback)
{
    editing_ = item;
    const ui::TextField &f = field(item);
    prompt_.set_title(item == kHost ? "Server address" : item == kPort   ? "Port"
                                                     : item == kFolder ? "Folder"
                                                     : item == kUser   ? "User name"
                                                                       : "Password");
    char port[16];
    std::snprintf(port, sizeof(port), "%d", connection_.default_port());
    prompt_.field.set_placeholder(item == kPort ? port : item == kFolder ? "/" : "");
    prompt_.field.set_helper(item == kPort     ? "Leave empty for the usual port"
                             : item == kHost   ? "The NAS's IP address, like 192.168.1.20"
                             : item == kFolder ? "Like /games"
                                               : "");
    prompt_.style.password = item == kPassword;
    prompt_.style.max_length = item == kPort ? 5 : item == kFolder ? 200 : 128;
    if (item == kPort)
        prompt_.keyboard.set_layouts({ui::KeyboardLayout::numeric()});
    else
        prompt_.keyboard.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()});
    std::string initial = f.text();
    prompt_.open(feedback, initial);
    prompt_.field.set_error("");
}

void ConnectionPage::accept(app::Feedback &feedback)
{
    std::string text = prompt_.text();
    std::string error;
    Connection next = connection_;
    switch (editing_)
    {
    case kHost:
        error = normalise_host(&text);
        next.host = text;
        break;
    case kPort:
        error = parse_port(text, &next.port);
        break;
    case kFolder:
        error = normalise_folder(&text);
        next.folder = text;
        break;
    case kUser:
        next.username = text;
        break;
    case kPassword:
        next.password = text;
        break;
    default:
        break;
    }
    if (!error.empty())
    {
        // Back to the keyboard with what was typed and why it was refused.
        prompt_.open(feedback, prompt_.text());
        prompt_.field.set_error(error);
        feedback.play(audio::Cue::error);
        return;
    }
    connection_ = next;
    store();
}

void ConnectionPage::store()
{
    save_error_ = connection_.save();
    sync_fields();
    ++revision_;
    // An earlier result described other settings.
    probe_.reset();
    local_result_ = {};
    heard_ = ProbeResult::State::idle;
}

void ConnectionPage::test_quietly()
{
    if (!connection_.configured() || probe_.running())
        return;
    local_result_ = {};
    probe_.start(connection_);
}

void ConnectionPage::test(app::Feedback &feedback)
{
    press_.trigger();
    if (probe_.running())
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        return; // already testing
    }
    if (!connection_.configured())
    {
        local_result_ = {ProbeResult::State::failed, "Enter your NAS's address first",
                         "Put the NAS's IP address in Server address, then test again."};
        heard_ = ProbeResult::State::failed;
        feedback.play(audio::Cue::error);
        refusal_.trigger();
        return;
    }
    local_result_ = {};
    probe_.start(connection_, true); // and count the games (silently, as its answer)
}

void ConnectionPage::browse(app::Feedback &feedback)
{
    press_.trigger();
    if (!connection_.configured())
    {
        local_result_ = {ProbeResult::State::failed, "Enter your NAS's address first",
                         "Put the NAS's IP address in Server address, then browse the NAS's folders."};
        heard_ = ProbeResult::State::failed;
        feedback.play(audio::Cue::error);
        refusal_.trigger();
        return;
    }
    picker_.open(connection_, connection_.folder, feedback);
}

void ConnectionPage::update(float dt)
{
    age_ += dt;
    probe_.poll();
    const bool editing = prompt_.is_open() || type_.is_open() || picker_.is_open();
    type_.set_active(focus_ == kType && !prompt_.is_open());
    test_focus_.target = focus_ == kTest && !editing ? 1.0f : 0.0f;
    test_focus_.update(dt, acrylic().omega);
    browse_focus_.target = focus_ == kBrowse && !editing ? 1.0f : 0.0f;
    browse_focus_.update(dt, acrylic().omega);
    picker_.update(dt);
    press_.update(dt, 10.0f);
    for (int i = kHost; i <= kPassword; ++i)
    {
        fields_[i].set_active(focus_ == i && !editing);
        fields_[i].set_disabled(locked() || unused(i));
        fields_[i].update(dt);
    }
    type_.update(dt);
    prompt_.update(dt);
    refusal_.update(dt, 9.0f);
}

void ConnectionPage::draw(gfx::DrawList &list, std::uint32_t glass, float clock, float t) const
{
    const ui::Theme &theme = acrylic();
    ui::Painter paint(list, fonts_, theme, glass);
    const float content = tween::smoothstep(tween::clamp01((t - 0.5f) / 0.5f));
    if (content <= 0.001f)
        return;

    list.push_opacity(content);
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 28.0f * (1.0f - content));

    if (!embedded_)
        paint.heading("Connection", kMargin - 2, 128, 64, paint.page_text());
    if (locked())
    {
        // A lock instead of the subtitle: what holds the settings, and how
        // to free them.
        const float x = kMargin;
        list.rounded_rect({x, 163.0f, 18.0f, 13.0f}, 3.0f, theme.warning);
        list.bordered_rect({x + 3.5f, 152.0f, 11.0f, 14.0f}, 5.5f, gfx::Color::rgb(0, 0.0f), 2.5f,
                           theme.warning);
        paint.body(ui::fit_label(paint, locked_, 24.0f, 1700.0f), x + 30.0f, 180, 24,
                   theme.warning);
    }
    else
    {
        paint.body("Where NASBridge finds your games", kMargin, 180, 24,
                   paint.page_text_muted());
    }

    ui::Canvas canvas{list, fonts_, glass, clock};
    list.push_transform(1.0f, 0.0f, 0.0f, ui::shake(refusal_.value, clock, 8.0f), 0.0f);
    list.push_opacity(locked() ? 0.45f : 1.0f);
    type_.draw(canvas);
    list.pop_opacity();
    for (int i = kHost; i <= kPassword; ++i)
        fields_[i].draw(canvas);
    {
        ui::Look look;
        look.focus = test_focus_.value;
        look.press = focus_ == kTest ? press_.value : 0.0f;
        const bool testing = probe_.running();
        paint.button(test_button_, testing ? "Testing..." : "Test connection",
                     ui::ButtonKind::primary, look);
    }
    {
        // The folder button: the File Browser's folder, on a plain button.
        ui::Look look;
        look.focus = browse_focus_.value;
        look.press = focus_ == kBrowse ? press_.value : 0.0f;
        look.disabled = locked();
        paint.button(browse_button_, "", ui::ButtonKind::secondary, look);
        const float sink = 2.0f * look.press;
        draw_folder_icon(list, browse_button_.cx(), browse_button_.cy() + sink, 34.0f,
                         locked() ? 0.4f : 1.0f);
    }
    list.pop_transform();

    draw_panel(list, paint, glass, clock);

    // While the keyboard is open, its buttons; otherwise the page's.
    const ui::Hint typing[] = {{ui::Button::options, "Done"}, {ui::Button::circle, "Cancel"}};
    const ui::Hint hints[] = {{ui::Button::dpad, "Move"},
                              {ui::Button::cross, focus_ == kTest     ? "Test"
                                                  : locked()          ? "Locked"
                                                  : unused(focus_)    ? "Not used"
                                                  : focus_ == kType   ? "Choose"
                                                  : focus_ == kBrowse ? "Browse"
                                                                      : "Edit"},
                              {ui::Button::l2, "Section", ui::Button::r2},
                              {ui::Button::circle, "Back"}};
    // The tab hint only inside the Settings screen.
    const ui::Hint plain[] = {hints[0], hints[1], hints[3]};
    // While the keyboard is open its hints are drawn over its veil, below.
    if (!prompt_.is_open() && embedded_)
        ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), hints, 4, 1824, true);
    else if (!prompt_.is_open())
        ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), plain, 3, 1824, true);

    list.pop_transform();
    list.pop_opacity();

    // Modal layers last, so they sit over everything.
    list.push_opacity(content);
    if (type_.visible())
        type_.draw_popover(canvas);
    list.pop_opacity();
    if (prompt_.visible())
        prompt_.draw(canvas);
    if (prompt_.is_open())
        ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), typing, 2, 1824, true);
    if (picker_.visible())
        picker_.draw(list, glass, clock);
}

void ConnectionPage::draw_panel(gfx::DrawList &list, ui::Painter &paint,
                                std::uint32_t glass, float clock) const
{
    (void)glass;
    const ui::Theme &theme = acrylic();
    constexpr float kPad = 40.0f;
    const float x = kPanel.x + kPad;
    const float w = kPanel.w - kPad * 2.0f;

    const std::vector<std::string> about = wrap(
        paint,
        connection_.type == ConnectionType::smb
            ? "Turn on SMB in your NAS's settings, then enter the NAS's "
              "address and sign-in here, and pick the games folder: its first part is the share. "
              "NASBridge only reads from your NAS: NASBridge never changes or deletes anything there."
        : connection_.type == ConnectionType::nfs
            ? "Turn on NFS in your NAS's settings and export the games share to this PS5, then "
              "enter the NAS's address here and pick the games folder (Unraid: /mnt/user/...). "
              "NASBridge only reads from your NAS: NASBridge never changes or deletes anything there."
            : "Turn on WebDAV in your NAS's settings, then enter the NAS's address here. "
              "Synology's WebDAV Server uses port 5005. NASBridge only reads from your NAS: "
              "NASBridge never changes or deletes anything there.",
        22.0f, w);
    constexpr float kLine = 32.0f;

    // ---- the test's result ----
    const ProbeResult result = test_result();
    std::string headline;
    std::string detail;
    gfx::Color dot = theme.text_muted;
    switch (result.state)
    {
    case ProbeResult::State::idle:
        headline = "Not tested yet";
        detail = "Test connection checks these settings with your NAS.";
        break;
    case ProbeResult::State::running:
    {
        int done = 0;
        int total = 0;
        probe_.progress(&done, &total);
        if (total > 0)
        {
            headline = "Counting games...";
            detail = "Looked in " + std::to_string(done) + " of " + std::to_string(total) +
                     (total == 1 ? " folder." : " folders.");
        }
        else
        {
            headline = "Testing...";
            detail = "Asking " + connection_.display_url() + " for the NAS's folder list.";
        }
        dot = theme.primary;
        break;
    }
    case ProbeResult::State::ok:
        headline = result.headline;
        detail = result.detail;
        dot = theme.success;
        break;
    case ProbeResult::State::failed:
        headline = result.headline;
        detail = result.detail;
        dot = theme.danger;
        break;
    }
    const std::vector<std::string> detail_lines = wrap(paint, detail, 21.0f, w);
    constexpr float kDetailLine = 30.0f;

    float height = 176.0f + static_cast<float>(about.size()) * kLine;
    height += 60.0f + 38.0f + 36.0f + static_cast<float>(detail_lines.size()) * kDetailLine;
    if (!save_error_.empty())
        height += 44.0f;
    paint.panel({kPanel.x, kPanel.y, kPanel.w, height});

    float y = kPanel.y + 62.0f;
    paint.label("Games are read from", x, y, 20, theme.text_muted);
    y += 42.0f;
    const std::string url = connection_.display_url();
    paint.body(ui::fit_label(paint, url, 26.0f, w), x, y, 26,
               connection_.configured() ? theme.text : theme.text_muted);
    y += 34.0f;
    list.rounded_rect({x, y, w, 1.0f}, 0.0f, theme.light.with_alpha(0.25f));
    y += 46.0f;
    for (const std::string &line : about)
    {
        paint.body(line, x, y, 22, theme.text_muted);
        y += kLine;
    }

    // Connection test.
    y += 14.0f;
    list.rounded_rect({x, y, w, 1.0f}, 0.0f, theme.light.with_alpha(0.25f));
    y += 46.0f;
    paint.label("Connection test", x, y, 20, theme.text_muted);
    y += 40.0f;
    float pulse = 1.0f;
    if (result.state == ProbeResult::State::running)
        pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(clock * 6.0f));
    list.circle(x + 7.0f, y - 8.0f, 7.0f, dot.with_alpha(pulse));
    paint.body(ui::fit_label(paint, headline, 24.0f, w - 28.0f), x + 28.0f, y, 24, theme.text);
    y += 36.0f;
    for (const std::string &line : detail_lines)
    {
        paint.body(line, x, y, 21, theme.text_muted);
        y += kDetailLine;
    }

    if (!save_error_.empty())
    {
        y += 10.0f;
        paint.body(ui::fit_label(paint, "Couldn't save: " + save_error_, 21.0f, w), x, y, 21,
                   theme.danger);
    }
}

} // namespace hui::concepts
