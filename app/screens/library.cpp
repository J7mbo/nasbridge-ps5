// NASBridge - The library and the game page.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   - Loading screen: the first kLoadingSeconds after launch. Behind it the
//     connection test runs (for the dot beside the server's address) and the
//     NAS's games folder is read (nas_library.cpp).
//   - Library: a header and a grid of square covers over the waves backdrop,
//     whose colours ease toward the focused game's cover. Game folders show
//     their sce_sys/icon0.png and the name from sce_sys/param.json as they
//     arrive; images (.exfat, .ffpfsc) the same, read from inside them.
//   - Filter tabs (L2 / R2): every game, the ones not on the PS5 yet, and the
//     installed ones (NASBridge can't check the PS5 yet: everything counts as
//     not installed).
//   - Game page (Cross on a cover): the game's key art (sce_sys/pic0.png,
//     fetched when the page opens) full screen, its title and details, and a
//     frosted Acrylic action panel. The cover travels from its grid cell to
//     the page; Circle reverses it.
//   - Settings (Options): the connection, copying and debug sections; see
//     settings_page.cpp (the connection itself is connection_page.cpp).
//     Leaving it after a change re-reads the NAS.
//
// The PC preview has no NAS: without saved settings it shows the UI kit's
// invented catalogue instead, so the snapshots have something to draw.

#include "concepts/concepts.hpp"
#include "concepts/app_settings.hpp"
#include "concepts/connection_page.hpp"
#include "concepts/settings_page.hpp"
#include "concepts/keep_awake.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas_library.hpp"
#include "concepts/nas_probe.hpp"
#include "concepts/notify.hpp"
#include "concepts/queue_panel.hpp"
#include "concepts/storage.hpp"
#include "concepts/transfer.hpp"

#include "core/tween.hpp"
#include "ui/components.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "platform/ps5/system.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

// ---- layout (1920 x 1080 virtual canvas, 96 px safe margins) ----
constexpr float kMargin = 96.0f;
// Tall enough that the kit counts two rows as on screen: moving down then
// brings the focus to the lower row and scrolls a row at a time, so the last
// row ends at the bottom with no empty row under it.
const Rect kGrid{kMargin, 230.0f, 1728.0f, 732.0f};
const Rect kPageCover{kMargin, 150.0f, 300.0f, 300.0f};
const Rect kPanel{1236.0f, 490.0f, 588.0f, 400.0f};
constexpr float kCoverRadius = 14.0f;
constexpr float kLoadingMin = 0.8f;   // the loading screen never flashes shorter
constexpr float kLoadingSaved = 8.0f; // with a saved library, the NAS check waits this long
constexpr float kLoadingDemo = 2.0f;  // the PC preview's demo catalogue
constexpr int kUploadsPerFrame = 2;     // covers sent to the GPU per frame
const Rect kTabs{372.0f, 98.0f, 820.0f, 60.0f};

const Color kWhite = Color::rgb(0xffffff);
const Color kBlack = Color::rgb(0x000000);
// A game without a cover yet: a quiet violet that suits Acrylic.
const Color kPlainDark = Color::rgb(0x150f26);
const Color kPlainMid = Color::rgb(0x3b2f63);

constexpr const char *kTechniques[] = {
    "GridView of square covers with a gliding focus",
    "Waves backdrop eased toward the focused cover's palette",
    "Shared element: the cover travels from its cell to the game page",
    "Frosted Acrylic action panel over full-screen key art",
    "Filter tabs on L2 / R2 that keep the focused game when they can",
    "Connection screen: a dropdown, text fields and the on-screen keyboard",
    "Covers and names read from the NAS on a background thread",
};

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

Rect mix_rect(const Rect &a, const Rect &b, float t)
{
    return {tween::lerp(a.x, b.x, t), tween::lerp(a.y, b.y, t), tween::lerp(a.w, b.w, t),
            tween::lerp(a.h, b.h, t)};
}

Color from_tone(const float tone[3])
{
    return Color{tone[0], tone[1], tone[2], 1.0f};
}

std::string gigabytes(double gb)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", gb);
    return text;
}

constexpr float kSpaceSeconds = 3.0f; // how often the free space and the drives are asked again
constexpr float kNoticeSeconds = 5.0f; // how long a refusal's reason stays on the page
constexpr float kAwakeSeconds = 60.0f;  // how often the PS5's idle timer is reset during a copy

// "12.3 of 85.0 GB  ·  98 MB/s  ·  14 min left"
std::string copy_progress(const QueueEntry &item, double rate)
{
    char text[160];
    const double done = static_cast<double>(item.done) / 1e9;
    const double total = static_cast<double>(item.size) / 1e9;
    std::snprintf(text, sizeof(text), "%.1f of %.1f GB", done, total);
    std::string line = text;
    if (rate > 1.0)
    {
        std::snprintf(text, sizeof(text), "  \u00b7  %.0f MB/s", rate / 1e6);
        line += text;
        const double seconds =
            static_cast<double>(item.size - std::min(item.done, item.size)) / rate;
        if (seconds < 60.0)
            std::snprintf(text, sizeof(text), "  \u00b7  under a minute left");
        else if (seconds < 3600.0)
            std::snprintf(text, sizeof(text), "  \u00b7  %d min left",
                          static_cast<int>(seconds / 60.0 + 0.5));
        else
            std::snprintf(text, sizeof(text), "  \u00b7  %d h %02d min left",
                          static_cast<int>(seconds / 3600.0), static_cast<int>(seconds / 60.0) % 60);
        line += text;
    }
    return line;
}

// One game in the library, from the NAS or (PC preview) the demo catalogue.
struct Game
{
    std::string title;
    std::string label; // above the title on the game page
    std::string blurb; // under the details on the game page
    std::string format;
    std::string title_id;
    std::string version;
    std::string note;     // why the details or cover are missing
    double size_gb = 0.0; // 0: unknown
    bool on_ps5 = false;
    std::uint32_t cover = 0; // texture; 0: none (yet)
    Rect uv = gfx::kFullUv;
    bool owns_cover = false; // uploaded by us (deleted on reload)
    Color dark = kPlainDark;
    Color mid = kPlainMid;
    Color accent = acrylic().primary;
    bool from_nas = false;
    NasGame source; // where it is on the NAS (for the key art)
};

// Sends a picture to the GPU; call on the GL (UI) thread only.
GLuint upload_texture(const Image &image)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, image.rgba.data());
    // One level only: mip chains leave ps5-opengl's batched fast path.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

// The library's tabs, left to right.
enum Filter
{
    kAll,
    kNotInstalled,
    kInstalled,
    kFilters,
};

// arriving: just copied, and ShadowMountPlus is adding it to the home screen
// (it counts as installed already).
bool passes(Filter filter, const Game &game, bool arriving)
{
    return filter == kAll || (filter == kInstalled) == (game.on_ps5 || arriving);
}

// Where the games come from, and how far reading them has got.
enum class Source
{
    demo,    // PC preview without a NAS: the kit's invented catalogue
    unset,   // no server address yet
    loading, // reading the NAS
    ready,   // the list is in (covers may still be arriving)
    failed,  // the NAS couldn't be read: failure_ says why
};

#if !defined(__PROSPERO__)
// The demo catalogue's invented details (PC preview only).
Game demo_game(const demo::Item &item, int index)
{
    static constexpr float kSizes[] = {42.1f, 87.4f, 18.6f, 61.3f, 9.2f,  55.0f, 96.8f, 74.9f,
                                       33.7f, 12.4f, 48.0f, 7.9f,  66.2f, 29.5f, 81.0f, 15.3f,
                                       39.8f, 22.6f, 58.4f, 11.1f, 44.9f, 70.2f, 26.7f, 52.3f};
    Game game;
    game.title = item.title;
    game.label = item.genre;
    game.blurb = item.blurb;
    game.format = index % 3 == 1 ? "Game folder" : "exFAT image";
    char text[16];
    std::snprintf(text, sizeof(text), "PPSA%05d", 1000 + (index * 1373) % 8000);
    game.title_id = text;
    std::snprintf(text, sizeof(text), "01.00%d.000", index % 7);
    game.version = text;
    game.size_gb = kSizes[static_cast<std::size_t>(index) % std::size(kSizes)];
    game.on_ps5 = index == 1 || index == 4 || index == 9 || index == 15;
    game.cover = item.cover;
    game.uv = gfx::kCanvasUv;
    game.dark = item.dark;
    game.mid = item.mid;
    game.accent = item.accent;
    return game;
}
#endif

class Library final : public app::Concept
{
  public:
    explicit Library(app::Context &context) : context_(context)
    {
        grid_.style.theme = acrylic();
        grid_.style.columns = 6;
        grid_.style.card.art_aspect = 1.0f;
        grid_.style.card.text = ui::CardText::below;
        grid_.style.card.title_size = 23.0f;
        grid_.style.card.subtitle_size = 19.0f;
        grid_.style.card.badge_corner = ui::CardCorner::top_right;
        grid_.style.card.glow = true;
        grid_.set_bounds(kGrid);

        tabs_.style.theme = acrylic();
        tabs_.style.kind = ui::TabKind::pill;
        tabs_.style.on_page = true;
        tabs_.style.text_size = 22.0f;
        tabs_.style.badge_size = 16.0f;
        tabs_.style.height = 52.0f;
        tabs_.style.focus_ring = false; // the covers hold the focus; L2 / R2 turn the tabs
        tabs_.set_tabs({{"All", 0}, {"Not installed", 0}, {"Installed", 0}});

        // Where copies go: Internal storage, or a drive plugged into the PS5,
        // as a list that opens from the "Destination" button. Every launch starts
        // on Internal storage.
        places_.style.theme = acrylic();
        places_.style.label = ui::SelectLabel::none;
        places_.style.scrim = 0.35f;
        places_.set_limits({0.0f, 0.0f, 1920.0f, 1080.0f});
        destination_ = kInternalGames;

        // A copy NASBridge was in the middle of last time comes back paused.
        transfers_.load();
        queue_ = transfers_.queue();
        removal_ = transfers_.removal();
        confirm_.style.theme = acrylic();
        tabs_.set_bounds(kTabs);
        tabs_.set_focused(false);
        tabs_.set_active(kAll, true);

        for (int i = 0; i < 4; ++i)
            tint_[i].snap(palette(Game{}, i));
        page_.snap(0.0f);
        settings_.snap(0.0f);

        loader_.style.theme = acrylic();
        loader_.style.layout = ui::LoadingLayout::center;
        loader_.style.indicator = ui::LoadingIndicator::bar; // centred, and it shows real progress
        loader_.style.percent = false;
        loader_.style.tint = Color::rgb(0x000000, 0.45f); // the waves show through, dimmed
        loader_.style.fade_in = 0.0f;                     // covers the very first frame
        loader_.style.fade_out = 0.45f;
        loader_.style.tips_by_hand = false;
        loader_.style.close_on_continue = false;
        loader_.title = "NASBridge";
        loader_.subtitle = "Getting your library ready";
        loader_.set_progress(-1.0f);

        start_reading();
    }

    ~Library() override
    {
        nas_.stop();
        art_.stop();
        release_covers();
        release_key_art();
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "library",
            "NASBridge",
            "Browse and copy games from your NAS",
            "app/screens/library.cpp",
            audio::SoundSet::glass,
            acrylic().primary,
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        grid_.enter();
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        update_frame(input, dt, feedback);
        // Settings > Sound off: nothing this frame makes a sound. The library
        // is the last to add any (the shell's welcome comes before it); the
        // controller's rumble isn't a sound and stays.
        if (!sounds_on())
            feedback.cues.clear();
    }

    void update_frame(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        age_ += dt;
        clock_ += dt;
        space_ = storage_.latest();
        sync_installed();
        queue_ = transfers_.queue();
        if (source_ == Source::demo && queue_panel_.visible() && queue_.entries.empty())
            queue_ = demo_queue();
        removal_ = transfers_.removal();
        follow_drives();
        // The free space and the game list change when a game finishes, the
        // queue stops or changes, or a delete ends: ask again.
        const std::string mark = std::to_string(static_cast<int>(queue_.state)) + "|" +
                                 std::to_string(queue_.entries.size()) + "|" +
                                 queue_.finished_entry + "|" +
                                 std::to_string(queue_.discarding.size()) + "|" +
                                 std::to_string(static_cast<int>(removal_.state));
        if (mark != queue_mark_)
        {
            queue_mark_ = mark;
            storage_.refresh();
            refresh_grid(); // a game just copied moves to Installed
        }
        notice_age_ += dt;
        // Queued games are copied from the NAS settings they were added with.
        if (queue_.running() && !queue_.entries.empty())
            connection_.set_locked("Locked while NASBridge copies " + queue_.entries.front().title +
                                   ". Let the queue finish, or empty it, to change these.");
        else if (!queue_.entries.empty())
            connection_.set_locked("Locked while games are in the queue. Resume it, or remove "
                                   "them, to change these.");
        else
            connection_.set_locked("");
        // While the queue copies or a delete runs, the PS5 isn't idle: it
        // stays out of rest mode (the first tick goes as soon as one starts),
        // unless that's turned off in Settings > Copying.
        if ((queue_.running() || removal_.busy()) && keep_awake_on())
        {
            awake_age_ += dt;
            if (awake_age_ >= kAwakeSeconds)
            {
                awake_age_ = 0.0f;
                keep_awake_tick();
            }
        }
        else
        {
            awake_age_ = kAwakeSeconds;
        }
        queue_panel_.update(dt, queue_);
        const std::string removed = queue_panel_.take_removed();
        if (!removed.empty())
            open_page_of(removed, feedback);
        queue_hint_ = queue_.entries.empty()
                          ? "Queue"
                          : "Queue (" + std::to_string(queue_.entries.size()) + ")";
        take_nas_events();
        take_key_art();

        if (loading_)
        {
            // While it shows, the loading screen takes every input.
            if (!loader_.is_open())
                loader_.show(feedback);
            loader_.handle(input, feedback);
            loading_age_ += dt;
            update_loader_text();
            if (loading_done())
            {
                loading_ = false;
                loader_.hide();
                age_ = 0.0f; // the library's entrance plays as the loader fades
                grid_.enter();
            }
        }
        else if (settings_open_)
        {
            if (!settings_screen_.handle(input, feedback))
            {
                settings_open_ = false;
                feedback.play(audio::Cue::back);
                if (connection_.revision() != revision_)
                    start_reading(); // the settings changed: read the NAS again
            }
        }
        else if (queue_panel_.is_open())
        {
            queue_panel_.handle(input, feedback, connection_.connection());
        }
        else if (page_open_)
        {
            update_page_input(input, feedback);
        }
        else
        {
            update_library_input(input, feedback);
        }

        grid_.update(dt);
        tabs_.update(dt);
        connection_.update(dt);
        settings_screen_.update(dt);
        // The waves keep the app's own blue on every tab, whatever is focused.
        for (int i = 0; i < 4; ++i)
        {
            tint_[i].target(palette(Game{}, i));
            tint_[i].update(dt, 4.0f);
        }
        page_.target = page_open_ ? 1.0f : 0.0f;
        page_.update(dt, page_open_ ? 9.0f : 12.0f);
        art_fade_.target = key_art_ != 0 && key_art_for_ == page_game_ ? 1.0f : 0.0f;
        art_fade_.update(dt, 5.0f);
        settings_.target = settings_open_ ? 1.0f : 0.0f;
        settings_.update(dt, settings_open_ ? 9.0f : 12.0f);
        loader_.update(dt);
        for (int i = 0; i < kActions; ++i)
        {
            action_focus_[i].target = i == action_ ? 1.0f : 0.0f;
            action_focus_[i].update(dt, acrylic().omega);
        }
        press_.update(dt, 10.0f);
        refusal_.update(dt, 9.0f);
        if (places_.is_open() && places_signature() != places_shown_)
            fill_places(); // a drive came or went while the list is open
        places_.update(dt);
        confirm_.update(dt);
        if (page_open_ && page_game_ >= 0 && page_game_ < static_cast<int>(games_.size()))
        {
            PageAction actions[kActions];
            const int count = page_actions(games_[static_cast<std::size_t>(page_game_)], actions);
            action_ = std::min(action_, count - 1);
        }
    }

    void draw(app::Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::waves;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = tint_[i].value();
        frame.backdrop.time = clock_;
        frame.glass = true;

        const float t = tween::clamp01(page_.value);
        if (t > 0.001f)
            draw_key_art(frame.scene, t);

        gfx::DrawList &list = frame.overlay;
        // The library has faded out by halfway and the page's words arrive
        // after it, so the two never show through each other. The Connection
        // screen comes and goes the same way.
        const float s = tween::clamp01(settings_.value);
        const float library = (1.0f - tween::smoothstep(tween::clamp01(t * 2.0f))) *
                              (1.0f - tween::smoothstep(tween::clamp01(s * 2.0f)));
        if (library > 0.001f && loader_.opacity() < 0.999f)
            draw_library(list, frame.glass_texture, library);
        if (queue_panel_.visible())
            queue_panel_.draw(list, frame.glass_texture, clock_, queue_,
                              [this](const std::string &entry)
                              {
                                  QueuePanel::Cover cover;
                                  for (const Game &game : games_)
                                      if (game.from_nas && game.source.entry == entry)
                                      {
                                          cover.texture = game.cover;
                                          cover.uv = game.uv;
                                          cover.top = game.mid;
                                          cover.bottom = game.dark;
                                      }
                                  return cover;
                              });
        if (t > 0.001f)
            draw_page(list, frame.glass_texture, t);
        if (places_.visible())
        {
            ui::Canvas canvas{list, context_.fonts, frame.glass_texture, clock_};
            places_.draw_popover(canvas);
        }
        if (confirm_.visible())
        {
            ui::Canvas canvas{list, context_.fonts, frame.glass_texture, clock_};
            confirm_.draw(canvas);
        }
        if (s > 0.001f)
            settings_screen_.draw(list, frame.glass_texture, clock_, s);
        if (loader_.visible())
        {
            ui::Canvas canvas{list, context_.fonts, frame.glass_texture, clock_};
            loader_.draw(canvas);
        }
    }

    std::span<const app::TourStep> tour() const override
    {
        static const app::TourStep kTour[] = {
            {1.2f, 0, Direction::none, "loading"},
            {2.4f, 0, Direction::none},
            {1.2f, 0, Direction::none, "library"},
            {0.25f, 0, Direction::right},
            {0.25f, 0, Direction::right},
            {0.25f, 0, Direction::down},
            {0.9f, 0, Direction::none, "library-focus"},
            {0.1f, action_bit(Action::confirm)},
            {0.12f, 0, Direction::none, "opening"},
            {1.0f, 0, Direction::none, "game"},
            {0.3f, 0, Direction::down},
            {0.5f, 0, Direction::none, "game-destination"},
            {0.2f, action_bit(Action::back)},
            {1.0f, 0, Direction::none, "back"},
            {0.25f, 0, Direction::up},
            {0.25f, 0, Direction::left},
            {0.6f, 0, Direction::none},
            {0.1f, action_bit(Action::confirm)},
            {1.2f, 0, Direction::none, "game-installed"},
            {0.2f, action_bit(Action::back)},
            {1.0f, 0, Direction::none},
            {0.1f, action_bit(Action::jump_next)},
            {0.9f, 0, Direction::none, "filter-not-installed"},
            {0.1f, action_bit(Action::jump_next)},
            {0.9f, 0, Direction::none, "filter-installed"},
            {0.1f, action_bit(Action::jump_next)},
            {0.4f, 0, Direction::none},
            {0.1f, action_bit(Action::jump_prev)},
            {0.1f, action_bit(Action::jump_prev)},
            {0.6f, 0, Direction::none},
            {0.1f, action_bit(Action::menu)},
            {1.0f, 0, Direction::none, "connection"},
            {0.1f, action_bit(Action::confirm)},
            {0.7f, 0, Direction::none, "connection-type"},
            {0.1f, action_bit(Action::back)},
            {0.4f, 0, Direction::none},
            {0.25f, 0, Direction::down},
            {0.1f, action_bit(Action::confirm)},
            {0.9f, 0, Direction::none, "connection-keyboard"},
            {0.1f, action_bit(Action::back)},
            {0.5f, 0, Direction::none},
            {0.25f, 0, Direction::down},
            {0.25f, 0, Direction::down},
            {0.6f, 0, Direction::none, "connection-sign-in"},
            {0.25f, 0, Direction::down},
            {0.1f, action_bit(Action::confirm)},
            {0.8f, 0, Direction::none, "connection-test"},
            {0.1f, action_bit(Action::jump_next)},
            {0.9f, 0, Direction::none, "settings-copying"},
            {0.25f, 0, Direction::right},
            {0.6f, 0, Direction::none, "settings-connections-3"},
            {0.25f, 0, Direction::left},
            {0.1f, action_bit(Action::jump_next)},
            {0.9f, 0, Direction::none, "settings-sound"},
            {0.1f, action_bit(Action::jump_next)},
            {0.9f, 0, Direction::none, "settings-debug"},
            {0.1f, action_bit(Action::confirm)},
            {0.6f, 0, Direction::none, "settings-debug-on"},
            {0.1f, action_bit(Action::confirm)},
            {0.3f, 0, Direction::none},
            {0.1f, action_bit(Action::jump_prev)},
            {0.1f, action_bit(Action::jump_prev)},
            {0.1f, action_bit(Action::jump_prev)},
            {0.5f, 0, Direction::none},
            {0.1f, action_bit(Action::back)},
            {1.0f, 0, Direction::none, "connection-closed"},
            {0.1f, action_bit(Action::north)},
            {6.0f, 0, Direction::none, "queue"},
        };
        return kTour;
    }

  private:
    static constexpr int kActions = 2;

    // PC preview only: a queue to show in the panel, copying at a wandering speed.
    QueueStatus demo_queue() const
    {
        QueueStatus q;
        q.state = QueueState::running;
        q.phase = TransferState::copying;
        q.rate = 1e6 * (44.0 + 6.0 * std::sin(clock_ * 1.3) + 3.0 * std::sin(clock_ * 3.7));
        q.connections = std::fmod(clock_, 9.0f) < 1.5f ? 3 : 4;
        q.conn_report = "1:t206 420M  2:t206 410M  3:r503 180M  4:t206 400M";
        // A folder game of many files (the longest lines the panel shows).
        q.files_total = 49439;
        q.files_done = 19774;
        for (std::size_t i = 0; i < 3 && i < games_.size(); ++i)
        {
            QueueEntry item;
            item.entry = games_[i].title;
            item.title = games_[i].title;
            item.destination = "/data/homebrew";
            item.size = (18ull + 9ull * i) << 30;
            item.started = i == 0;
            item.done = i == 0 ? item.size * 2 / 5 : 0;
            q.entries.push_back(item);
        }
        return q;
    }

    // The waves' four colours from a game's palette: two deep base tones and
    // two ribbons, kept dark enough for white text and frosted panels.
    static Color palette(const Game &game, int slot)
    {
        switch (slot)
        {
        case 0:
            return gfx::mix(game.dark, kBlack, 0.45f);
        case 1:
            return gfx::mix(game.dark, kBlack, 0.7f);
        case 2:
            return gfx::mix(game.mid, kBlack, 0.25f);
        default:
            return gfx::mix(game.accent, kBlack, 0.35f);
        }
    }

    // ---- where the games come from ----

    // (Re)reads the games: from the NAS, or the demo catalogue on the PC
    // preview when no NAS is set up. Also re-runs the connection test.
    void start_reading()
    {
        revision_ = connection_.revision();
        nas_.stop();
        art_.stop();
        release_covers();
        release_key_art();
        games_.clear();
        failure_ = {};
        ready_ = false;
        offline_ = false;
        progress_done_ = 0;
        progress_total_ = 0;
        progress_text_.clear();
        const Connection &nas = connection_.connection();
#if !defined(__PROSPERO__)
        if (!nas.configured())
        {
            source_ = Source::demo;
            for (std::size_t i = 0; i < context_.catalog.size(); ++i)
                games_.push_back(demo_game(context_.catalog[i], static_cast<int>(i)));
            refresh_grid();
            return;
        }
#endif
        if (!nas.configured())
        {
            source_ = Source::unset;
            refresh_grid();
            return;
        }
        // The saved library shows at once; the NAS is checked behind it.
        std::vector<NasGame> saved;
        has_saved_ = load_library(nas, &saved);
        for (const NasGame &found : saved)
            games_.push_back(nas_game(found));
        source_ = has_saved_ ? Source::ready : Source::loading;
        refresh_grid();
        connection_.test_quietly();
        nas_.start(nas, saved);
    }

    // When the loading screen may go: the library is complete, or the NAS
    // can't be reached, or (with a saved library on screen) the check has
    // had kLoadingSaved seconds and carries on in the background.
    bool loading_done() const
    {
        if (loading_age_ < kLoadingMin)
            return false;
        switch (source_)
        {
        case Source::demo:
            return loading_age_ >= kLoadingDemo;
        case Source::unset:
            return true;
        default:
            break;
        }
        if (ready_ || source_ == Source::failed || offline_ || !nas_.active())
            return true;
        return has_saved_ && loading_age_ >= kLoadingSaved;
    }

    void update_loader_text()
    {
        if (source_ == Source::demo || source_ == Source::unset)
        {
            loader_.subtitle = "Getting your library ready";
            loader_.set_progress(-1.0f);
            return;
        }
        loader_.subtitle = progress_text_.empty() ? "Getting your library ready" : progress_text_;
        if (progress_total_ > 0)
            loader_.set_progress(static_cast<float>(progress_done_) /
                                 static_cast<float>(progress_total_));
        else
            loader_.set_progress(-1.0f);
    }

    int index_of(const std::string &entry) const
    {
        for (std::size_t i = 0; i < games_.size(); ++i)
            if (games_[i].from_nas && games_[i].source.entry == entry)
                return static_cast<int>(i);
        return -1;
    }

    // Takes what the NAS reader has sent: saved covers, the checked list,
    // new covers, folder sizes, and how far it has got.
    void take_nas_events()
    {
        int uploads = 0;
        LibraryEvent event;
        while (uploads < kUploadsPerFrame && nas_.take(&event))
        {
            switch (event.kind)
            {
            case LibraryEvent::Kind::listed:
                take_list(event.games);
                break;
            case LibraryEvent::Kind::failed:
                failure_ = {ProbeResult::State::failed, event.headline, event.detail};
                if (games_.empty())
                {
                    source_ = Source::failed;
                    refresh_grid();
                }
                else
                {
                    offline_ = true; // keep showing the saved library
                }
                break;
            case LibraryEvent::Kind::cover:
            {
                const int index = index_of(event.entry);
                if (index >= 0)
                {
                    set_cover(index, event.image, event.tone);
                    ++uploads;
                }
                break;
            }
            case LibraryEvent::Kind::note:
            {
                const int index = index_of(event.entry);
                if (index >= 0)
                    games_[static_cast<std::size_t>(index)].note = event.detail;
                break;
            }
            case LibraryEvent::Kind::size:
            {
                const int index = index_of(event.entry);
                if (index >= 0)
                {
                    Game &game = games_[static_cast<std::size_t>(index)];
                    game.source.size = event.bytes;
                    game.source.size_known = true;
                    game.size_gb = static_cast<double>(event.bytes) / 1e9;
                    update_card(index);
                }
                break;
            }
            case LibraryEvent::Kind::progress:
                progress_done_ = event.done;
                progress_total_ = event.total;
                progress_text_ = event.detail;
                break;
            case LibraryEvent::Kind::ready:
                ready_ = true;
                break;
            case LibraryEvent::Kind::key_art: // comes through art_, not here
            case LibraryEvent::Kind::finished:
                break;
            }
        }
    }

    // The checked list replaces the shown one. Covers already on screen stay
    // (they're matched by the game's name on the NAS); gone games let go of
    // theirs; the focus stays on the same game when it's still there.
    void take_list(const std::vector<NasGame> &found)
    {
        const std::string focused =
            shown_.empty() ? std::string() : games_[static_cast<std::size_t>(focused_game())].source.entry;
        std::vector<Game> next;
        next.reserve(found.size());
        for (const NasGame &g : found)
        {
            Game game = nas_game(g);
            const int old = index_of(g.entry);
            if (old >= 0)
            {
                Game &before = games_[static_cast<std::size_t>(old)];
                game.cover = before.cover;
                game.owns_cover = before.owns_cover;
                game.uv = before.uv;
                game.dark = before.dark;
                game.mid = before.mid;
                game.accent = before.accent;
                before.cover = 0; // moved
                before.owns_cover = false;
            }
            next.push_back(std::move(game));
        }
        release_covers(); // what's left belongs to games that are gone
        // An open game page stays open on its game (it may have moved).
        const std::string paged = page_game_ < static_cast<int>(games_.size())
                                      ? games_[static_cast<std::size_t>(page_game_)].source.entry
                                      : std::string();
        const bool art_matches = key_art_for_ == page_game_;
        games_ = std::move(next);
        source_ = Source::ready;
        refresh_grid();
        if (!focused.empty())
        {
            for (std::size_t slot = 0; slot < shown_.size(); ++slot)
                if (games_[static_cast<std::size_t>(shown_[slot])].source.entry == focused)
                    grid_.set_focus(static_cast<int>(slot), true);
        }
        if (page_open_)
        {
            const int moved = index_of(paged);
            if (moved < 0)
            {
                page_open_ = false; // its game is gone
                page_game_ = 0;
            }
            else
            {
                page_game_ = moved;
            }
            key_art_for_ = art_matches && moved >= 0 ? moved : -1;
        }
    }

    void update_card(int index)
    {
        for (std::size_t slot = 0; slot < shown_.size(); ++slot)
            if (shown_[slot] == index)
                grid_.item(static_cast<int>(slot)) = card_for(index);
    }

    Game nas_game(const NasGame &found) const
    {
        Game game;
        game.title = found.title;
        game.format = !found.image                              ? "Game folder"
                      : is_compressed_image_name(found.image_path) ? "Compressed image"
                                                                   : "exFAT image";
        game.label = game.format;
        game.title_id = found.title_id;
        game.version = found.version;
        game.size_gb = found.size_known ? static_cast<double>(found.size) / 1e9 : 0.0;
        const std::string root = connection_.connection().folder;
        game.blurb = (root == "/" ? "" : root) + "/" + found.entry + " on your NAS";
        game.note = found.note;
        game.from_nas = true;
        game.source = found;
        return game;
    }

    // Sends a cover to the GPU (this is the GL thread) and shows it.
    void set_cover(int index, const Image &image, const float tone[3][3])
    {
        const GLuint texture = upload_texture(image);

        Game &game = games_[static_cast<std::size_t>(index)];
        if (game.owns_cover && game.cover != 0)
        {
            const GLuint old = game.cover;
            glDeleteTextures(1, &old);
        }
        game.cover = texture;
        game.owns_cover = true;
        game.uv = gfx::kFullUv;
        game.dark = from_tone(tone[0]);
        game.mid = from_tone(tone[1]);
        game.accent = from_tone(tone[2]);
        update_card(index);
    }

    void release_key_art()
    {
        if (key_art_ != 0)
        {
            const GLuint texture = key_art_;
            glDeleteTextures(1, &texture);
        }
        key_art_ = 0;
        key_art_for_ = -1;
    }

    // The key art asked for when a game page opened.
    void take_key_art()
    {
        LibraryEvent event;
        while (art_.take(&event))
        {
            if (event.kind != LibraryEvent::Kind::key_art || !page_open_)
                continue;
            release_key_art();
            key_art_ = upload_texture(event.image);
            key_art_for_ = page_game_;
        }
    }

    void release_covers()
    {
        for (Game &game : games_)
            if (game.owns_cover && game.cover != 0)
            {
                const GLuint texture = game.cover;
                glDeleteTextures(1, &texture);
                game.cover = 0;
            }
    }

    // ---- the grid ----

    int page_game() const
    {
        return page_game_;
    }

    // The index in games_ of the focused cover (the grid counts only the
    // games the tab shows).
    int focused_game() const
    {
        return shown_[static_cast<std::size_t>(grid_.focus())];
    }

    // Copied, and on its way to the home screen; or off the home screen
    // with files still being deleted (or left by a delete that stopped).
    // Either way it counts as on the PS5: it can't be copied again yet.
    bool arriving(const Game &game) const
    {
        if (!game.from_nas || game.on_ps5)
            return false;
        if (removal_is(game) && removal_.leftover &&
            (removal_.state == TransferState::deleting || removal_.state == TransferState::failed))
            return true;
        return queue_.finished_entry == game.source.entry ||
               queue_.waiting(game.source.entry) != nullptr;
    }

    // Counts per tab, and the grid for the current one.
    void refresh_grid()
    {
        int counts[kFilters] = {};
        for (const Game &game : games_)
            for (int f = 0; f < kFilters; ++f)
                counts[f] += passes(static_cast<Filter>(f), game, arriving(game)) ? 1 : 0;
        for (int f = 0; f < kFilters; ++f)
            tabs_.tab(f).badge = counts[f];
        shown_.clear();
        show(filter_);
    }

    // Fills the grid with the games the filter lets through, keeping the
    // focused game when it is still there.
    void show(Filter filter)
    {
        const int keep = shown_.empty() ? -1 : focused_game();
        filter_ = filter;
        shown_.clear();
        std::vector<ui::CardItem> items;
        int focus = 0;
        for (int i = 0; i < static_cast<int>(games_.size()); ++i)
        {
            if (!passes(filter, games_[static_cast<std::size_t>(i)],
                        arriving(games_[static_cast<std::size_t>(i)])))
                continue;
            if (i == keep)
                focus = static_cast<int>(shown_.size());
            shown_.push_back(i);
            items.push_back(card_for(i));
        }
        grid_.set_items(std::move(items));
        grid_.set_focus(focus, true);
    }

    ui::CardItem card_for(int index) const
    {
        const Game &game = games_[static_cast<std::size_t>(index)];
        ui::CardItem card;
        card.title = game.title;
        card.subtitle =
            game.size_gb > 0.0 ? game.format + "  ·  " + gigabytes(game.size_gb) : game.format;
        card.badge = game.on_ps5 ? "ON PS5" : "";
        card.texture = game.cover;
        card.uv = game.uv;
        card.top = game.mid;
        card.bottom = game.dark;
        card.accent = game.accent;
        card.tag = index;
        return card;
    }

    void update_library_input(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::north))
        {
            queue_panel_.open(feedback);
            return;
        }
        if (input.is_pressed(Action::menu))
        {
            settings_open_ = true;
            settings_screen_.enter();
            feedback.play(audio::Cue::open);
            return;
        }
        const int turn = input.is_pressed(Action::jump_next)   ? 1
                         : input.is_pressed(Action::jump_prev) ? -1
                                                               : 0;
        if (turn != 0)
        {
            if (tabs_.step(turn, input, feedback) == ui::Event::changed)
                show(static_cast<Filter>(tabs_.active()));
            return;
        }
        if (shown_.empty())
            return; // nothing to move between or open
        const ui::Event event = grid_.handle(input, feedback);
        if (event == ui::Event::activated)
            open_page(feedback);
    }

    // Where the focused cell's cover is drawn: the cell also holds the title
    // under the cover, and the focused card is drawn larger and lifted, so
    // the flight to and from the page starts and lands exactly on the cover.
    Rect focused_cover_rect() const
    {
        const Rect cell = grid_.cell_rect(grid_.focus());
        const Rect art = ui::card_art(grid_.style.card, cell);
        ui::CardState focused;
        focused.focus = 1.0f;
        const float scale = ui::card_scale(grid_.style, grid_.style.card, focused);
        const float lift = ui::card_lift(grid_.style, grid_.style.card, focused);
        return {cell.cx() + (art.x - cell.cx()) * scale,
                cell.cy() + (art.y - cell.cy()) * scale - lift, art.w * scale, art.h * scale};
    }

    // A game's page, from the queue: the game gets the focus (on the All
    // tab, when the one shown leaves it out) and its page opens.
    void open_page_of(const std::string &entry, app::Feedback &feedback)
    {
        const int index = index_of(entry);
        if (index < 0)
            return;
        auto slot = std::find(shown_.begin(), shown_.end(), index);
        if (slot == shown_.end())
        {
            tabs_.set_active(kAll, true);
            show(kAll);
            slot = std::find(shown_.begin(), shown_.end(), index);
            if (slot == shown_.end())
                return;
        }
        grid_.set_focus(static_cast<int>(slot - shown_.begin()), true);
        open_page(feedback);
    }

    void open_page(app::Feedback &feedback)
    {
        page_open_ = true;
        page_game_ = focused_game();
        page_from_ = focused_cover_rect();
        action_ = 0;
        notice_age_ = kNoticeSeconds; // an old refusal belongs to another game
        for (int i = 0; i < kActions; ++i)
            action_focus_[i].snap(i == 0 ? 1.0f : 0.0f);
        feedback.play(audio::Cue::open);
        // The key art comes from the NAS while the page opens; it fades in
        // when it arrives (the waves show until then).
        const Game &game = games_[static_cast<std::size_t>(page_game_)];
        if (key_art_for_ != page_game_)
        {
            release_key_art();
            art_fade_.snap(0.0f);
            art_.stop();
            if (game.from_nas)
                art_.start_key_art(connection_.connection(), game.source);
        }
    }

    void update_page_input(const InputFrame &input, app::Feedback &feedback)
    {
        if (confirm_.is_open())
        {
            if (confirm_.handle(input, feedback) == ui::Event::activated && confirm_.choice() == 1)
                confirmed(feedback);
            return;
        }
        if (places_.is_open())
        {
            if (places_.handle(input, feedback) == ui::Event::changed)
            {
                const int picked = places_.index();
                if (picked >= 0 && picked < static_cast<int>(place_paths_.size()))
                {
                    destination_ = place_paths_[static_cast<std::size_t>(picked)];
                }
            }
            return;
        }
        if (input.is_pressed(Action::back))
        {
            page_open_ = false;
            page_from_ = focused_cover_rect();
            feedback.play(audio::Cue::back);
            return;
        }
        const Game &game = games_[static_cast<std::size_t>(page_game_)];
        PageAction actions[kActions];
        const int count = page_actions(game, actions);
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = action_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < count)
            {
                action_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                refusal_.trigger();
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            const PageAction &chosen = actions[std::clamp(action_, 0, count - 1)];
            if (!chosen.enabled)
            {
                if (chosen.command == Command::unqueue)
                    notice("Pause the queue first, then remove the game");
                // Refused: no room, the size isn't known yet, something else
                // is running, or a copy must be paused before it is deleted.
                feedback.play(audio::Cue::error);
                refusal_.trigger();
                return;
            }
            run(chosen.command, game, feedback);
        }
    }

    // ---- what the game page offers ----

    enum class Command
    {
        none,
        enqueue,     // add to the copy queue
        destination, // choose where copies go
        pause,       // pause the queue
        resume,      // resume the queue
        unqueue,     // take out of the queue (its copied files are deleted)
        remove,      // delete a game on the PS5
    };

    struct PageAction
    {
        std::string label;
        Command command = Command::none;
        bool enabled = true;
        bool primary = false;
    };

    // Where this game is in the queue (-1: not queued).
    int queued_at(const Game &game) const
    {
        return game.from_nas ? queue_.find(game.source.entry) : -1;
    }

    // The delete from the PS5 in hand is about this game.
    bool removal_is(const Game &game) const
    {
        return removal_.kind == TransferKind::remove && game.from_nas &&
               removal_.entry == game.source.entry;
    }

    // The game ShadowMountPlus has for this one (by title ID), if any.
    const SmpGame *ps5_game(const Game &game) const
    {
        if (game.title_id.empty())
            return nullptr;
        for (const SmpGame &smp : space_.games)
            if (smp.title_id == game.title_id && drive_connected(space_, smp.path))
                return &smp; // a game on a drive that was unplugged isn't there
        return nullptr;
    }

    // "On PS5" comes from ShadowMountPlus's list, once it has answered.
    void sync_installed()
    {
        if (!space_.games_known)
            return;
        bool changed = false;
        for (Game &game : games_)
        {
            if (!game.from_nas)
                continue;
            const bool on = ps5_game(game) != nullptr;
            if (on != game.on_ps5)
            {
                game.on_ps5 = on;
                changed = true;
            }
        }
        if (changed)
            refresh_grid();
    }

    // The buttons for this game, top first; returns how many (1 or 2).
    int page_actions(const Game &game, PageAction out[kActions]) const
    {
        const int at = queued_at(game);
        if (at == 0)
        {
            // The front of the queue: being copied, or waiting to carry on.
            if (queue_.running())
            {
                out[0] = {"Pause", Command::pause, true, true};
                out[1] = {"Remove (pause first)", Command::unqueue, false, false};
            }
            else
            {
                out[0] = {"Resume", Command::resume, true, true};
                out[1] = {"Remove from queue", Command::unqueue, true, false};
            }
            return 2;
        }
        if (at > 0)
        {
            out[0] = {"Remove from queue", Command::unqueue, true, false};
            return 1;
        }
        const bool discarding =
            game.from_nas && std::find(queue_.discarding.begin(), queue_.discarding.end(),
                                       game.source.entry) != queue_.discarding.end();
        if (discarding)
        {
            out[0] = {"Deleting copied files...", Command::none, false, false};
            return 1;
        }
        if (removal_is(game) && removal_.state == TransferState::deleting)
        {
            out[0] = {"Deleting...", Command::none, false, false};
            return 1;
        }
        if (removal_is(game) && removal_.state == TransferState::failed && removal_.leftover)
        {
            // Off the home screen with some of its files left: finish first.
            out[0] = {"Delete the rest", Command::remove, true, true};
            return 1;
        }
        if (removal_is(game) && removal_.state == TransferState::deleted && game.on_ps5)
        {
            // Until ShadowMountPlus's list catches up.
            out[0] = {"Deleted", Command::none, false, false};
            return 1;
        }
        if (game.on_ps5)
        {
            const bool other = removal_.busy();
            out[0] = {other ? "Another game is being deleted" : "Delete from PS5", Command::remove,
                      !other, false};
            return 1;
        }
        if (!game.from_nas)
        {
            out[0] = {"Demo game", Command::none, false, false};
            out[1] = {destination_label(), Command::destination, true, false};
            return 2;
        }
        if (queue_.finished_entry == game.source.entry ||
            queue_.waiting(game.source.entry) != nullptr)
        {
            out[0] = {"Copied", Command::none, false, false};
            return 1;
        }
        const CopyCheck check = copy_check(game);
        // The first game starts copying at once; later ones wait their turn.
        const char *label = queue_.entries.empty() ? "Download to PS5" : "Add to queue";
        switch (check)
        {
        case CopyCheck::too_big:
            label = "Not enough space";
            break;
        case CopyCheck::size_unknown:
            label = "Measuring size...";
            break;
        case CopyCheck::unavailable:
            label = "Drive not connected";
            break;
        case CopyCheck::not_writable:
            label = "Can't write here";
            break;
        case CopyCheck::checking:
            label = "Checking drive...";
            break;
        case CopyCheck::fits:
        case CopyCheck::space_unknown:
            break;
        }
        const bool blocked = check != CopyCheck::fits && check != CopyCheck::space_unknown;
        out[0] = {label, Command::enqueue, !blocked, !blocked};
        out[1] = {destination_label(), Command::destination, true, false};
        return 2;
    }

    void run(Command command, const Game &game, app::Feedback &feedback)
    {
        std::string why;
        bool ok = true;
        switch (command)
        {
        case Command::none:
            return;
        case Command::destination:
            open_places(feedback);
            return;
        case Command::remove:
            open_confirm(command, game, feedback);
            return;
        case Command::unqueue:
        {
            const int at = queued_at(game);
            if (at >= 0 && queue_.entries[static_cast<std::size_t>(at)].started)
            {
                open_confirm(command, game, feedback); // it has files to delete
                return;
            }
            ok = at >= 0 && transfers_.remove(at, &why);
            break;
        }
        case Command::enqueue:
            ok = transfers_.enqueue(connection_.connection(), game.source, game.title,
                                    destination_, &why);
            if (ok)
            {
                // Back to the library, with the queue open on the right to
                // show it going.
                queue_ = transfers_.queue();
                page_open_ = false;
                page_from_ = focused_cover_rect();
                press_.trigger();
                queue_panel_.open(feedback);
                return;
            }
            break;
        case Command::pause:
            ok = transfers_.pause();
            if (!ok)
                why = "The queue is already stopping";
            break;
        case Command::resume:
            ok = transfers_.resume(connection_.connection(), &why);
            break;
        }
        queue_ = transfers_.queue();
        if (!ok)
        {
            notice(why);
            feedback.play(audio::Cue::error);
            refusal_.trigger();
            return;
        }
        press_.trigger();
        feedback.play(audio::Cue::select);
    }

    void notice(const std::string &text)
    {
        notice_ = text;
        notice_age_ = 0.0f;
    }

    // Drives come and go: a chosen drive that was unplugged gives way to
    // Internal storage, and a queue paused for its drive carries on once the
    // drive is back.
    void follow_drives()
    {
        if (destination_ != kInternalGames && !drive_connected(space_, destination_))
        {
            notice(destination_name(destination_) +
                   " isn't connected. Copies go to Internal storage.");
            destination_ = kInternalGames;
        }
        if (queue_.drive_wait.empty())
            drive_resumed_.clear();
        if (queue_.drive_wait.empty() || queue_.drive_wait == drive_resumed_ ||
            !space_.drives_known || !drive_connected(space_, queue_.drive_wait))
            return;
        for (const Destination &place : space_.destinations)
            if (place.path == queue_.drive_wait && place.writable == 1)
            {
                drive_resumed_ = queue_.drive_wait; // once: a refusal waits for Resume
                std::string why;
                if (!transfers_.resume(connection_.connection(), &why))
                    notice(why);
                queue_ = transfers_.queue();
                return;
            }
    }

    // Asks before anything is deleted.
    void open_confirm(Command command, const Game &game, app::Feedback &feedback)
    {
        confirm_command_ = command;
        ui::DialogContent content;
        content.icon = ui::StatusKind::danger;
        if (command == Command::unqueue)
        {
            const int at = queued_at(game);
            const QueueEntry &item = queue_.entries[static_cast<std::size_t>(std::max(at, 0))];
            content.title = "Remove " + game.title + " from the queue?";
            content.body = "The " + gigabytes(static_cast<double>(item.done) / 1e9) +
                           " copied so far will be deleted from " +
                           destination_name(item.destination) +
                           ". The game on your NAS isn't touched.";
            content.buttons = {{"Cancel"}, {"Remove", ui::ButtonKind::primary, true}};
        }
        else
        {
            const SmpGame *smp = ps5_game(game);
            const std::string where = smp != nullptr && smp->source_type != "pkg"
                                          ? destination_name(smp->path)
                                          : "the PS5";
            content.title = "Delete " + game.title + " from the PS5?";
            content.body = "The game will be removed from the home screen and its files deleted from " +
                           where + ". The copy on your NAS isn't touched.";
            content.buttons = {{"Cancel"}, {"Delete", ui::ButtonKind::primary, true}};
        }
        confirm_.open(std::move(content), feedback);
    }

    void confirmed(app::Feedback &feedback)
    {
        if (page_game_ < 0 || page_game_ >= static_cast<int>(games_.size()))
            return;
        const Game &game = games_[static_cast<std::size_t>(page_game_)];
        std::string why;
        bool ok = false;
        if (confirm_command_ == Command::unqueue)
        {
            const int at = queued_at(game);
            ok = at >= 0 && transfers_.remove(at, &why);
            if (at < 0)
                why = "That game left the queue already";
        }
        else
        {
            ok = transfers_.remove_installed(game.title_id, game.source.entry, game.title, &why);
        }
        queue_ = transfers_.queue();
        removal_ = transfers_.removal();
        if (!ok)
        {
            notice(why);
            feedback.play(audio::Cue::error);
            refusal_.trigger();
        }
    }

    // The button that opens the list: what is chosen now.
    std::string destination_label() const
    {
        return "Destination: " + destination_name(destination_);
    }

    // Opens the list of places a copy can go, on the current one. Internal
    // storage is always there; a drive only while it's plugged in.
    void open_places(app::Feedback &feedback)
    {
        fill_places();
        places_.open(feedback);
    }

    // Fills the list from the latest storage answer. Called again while it's
    // open whenever that changes, so a drive plugged in or out shows at once.
    void fill_places()
    {
        std::vector<Destination> places = space_.destinations;
        if (places.empty())
        {
            // No answer yet: Internal storage, numbers unknown.
            Destination internal;
            internal.path = kInternalGames;
            places.push_back(internal);
        }
        std::vector<ui::SelectOption> options;
        place_paths_.clear();
        int current = -1;
        for (const Destination &place : places)
        {
            ui::SelectOption option(destination_name(place.path));
            char line[160];
            if (place.writable == 0)
                std::snprintf(line, sizeof(line), "Can't copy here (%s)", place.why.c_str());
            else
                std::snprintf(line, sizeof(line), "%.1f GB free  ·  %s",
                              static_cast<double>(place.free) / 1e9, place.path.c_str());
            option.description = line;
            option.disabled = place.writable == 0;
            if (place.path == destination_)
                current = static_cast<int>(options.size());
            options.push_back(option);
            place_paths_.push_back(place.path);
        }
        places_.set_options(std::move(options));
        places_.set_index(current);
        // Anchored on the "Destination" button.
        const float x = kPanel.x + 40.0f;
        places_.set_bounds({x, kPanel.y + 206.0f + 92.0f, kPanel.w - 80.0f,
                            places_.preferred_height()});
        places_shown_ = places_signature();
    }

    // Which places the list shows and whether each can be picked, to tell
    // when it needs filling again (free space changing alone doesn't: the
    // highlight would jump about during a copy).
    std::string places_signature() const
    {
        std::string text = destination_;
        for (const Destination &place : space_.destinations)
            text += "|" + place.path + (place.writable == 0 ? "!" : "");
        return text;
    }

    // The chosen place among those ShadowMountPlus lists; null when it isn't
    // listed (a drive that was unplugged, or no list yet).
    const Destination *destination() const
    {
        for (const Destination &place : space_.destinations)
            if (place.path == destination_)
                return &place;
        return nullptr;
    }

    enum class CopyCheck
    {
        fits,          // there's room for it
        too_big,       // it needs more than is free
        size_unknown,  // the folder hasn't been measured yet
        space_unknown, // the PS5 didn't say how much is free
        unavailable,   // the chosen drive isn't connected
        not_writable,  // NASBridge can't write there
        checking,      // NASBridge is still checking it can write there
    };

    CopyCheck copy_check(const Game &game) const
    {
        if (!space_.asked)
            return CopyCheck::space_unknown;
        std::uint64_t free = 0;
        if (const Destination *place = destination())
        {
            if (place->writable == 0)
                return CopyCheck::not_writable;
            if (place->writable < 0)
                return CopyCheck::checking;
            free = place->free;
        }
        else if (!space_.destinations.empty())
        {
            return CopyCheck::unavailable;
        }
        else if (destination_ == kInternalGames && space_.known)
        {
            free = space_.free; // no list from ShadowMountPlus: its own numbers
        }
        else
        {
            return CopyCheck::space_unknown;
        }
        if (game.size_gb <= 0.0)
            return CopyCheck::size_unknown;
        // What the queue will still write to this drive is spoken for.
        const std::uint64_t queued = queue_.queued_for(destination_);
        const double room = free > queued ? static_cast<double>(free - queued) : 0.0;
        return game.size_gb * 1e9 > room ? CopyCheck::too_big : CopyCheck::fits;
    }

    double destination_free_gb() const
    {
        const Destination *place = destination();
        return static_cast<double>(place != nullptr ? place->free : space_.free) / 1e9;
    }

    // ---- library ----
    void draw_library(gfx::DrawList &list, std::uint32_t glass, float alpha) const
    {
        const ui::Theme &theme = acrylic();
        ui::Painter paint(list, context_.fonts, theme, glass);
        const float in = tween::cubic_out(age_ / 0.45f);
        list.push_opacity(alpha * in);
        // The library steps back a little while the page opens over it.
        list.push_transform(1.0f - 0.04f * (1.0f - alpha), 960.0f, 540.0f, 0.0f,
                            20.0f * (1.0f - in));

        // ---- header ----
        paint.heading("Library", kMargin - 2, 150, 64, paint.page_text());
        const int shown = static_cast<int>(shown_.size());
        char count[64];
        if (source_ == Source::loading)
            std::snprintf(count, sizeof(count), "Reading your NAS...");
        else if (source_ == Source::unset || source_ == Source::failed)
            std::snprintf(count, sizeof(count), "Your NAS isn't connected");
        else if (filter_ == kAll)
            std::snprintf(count, sizeof(count), "%d %s on your NAS", shown,
                          shown == 1 ? "game" : "games");
        else if (filter_ == kNotInstalled)
            std::snprintf(count, sizeof(count), "%d not on this PS5 yet", shown);
        else
            std::snprintf(count, sizeof(count), "%d on this PS5", shown);
        paint.body(count, kMargin, 196, 24, paint.page_text_muted());

        // What's happening in the background, after the count: a pulsing
        // dot and the reader's latest step, or that the NAS is out of reach.
        std::string status;
        Color status_ink = paint.page_text_muted();
        bool pulsing = false;
        if (offline_)
        {
            status = "Couldn't reach your NAS: showing your saved library";
            status_ink = theme.danger;
        }
        else if (queue_.running() && !queue_.entries.empty())
        {
            const QueueEntry &head = queue_.entries.front();
            status = "Copying " + head.title;
            if (queue_.phase == TransferState::copying && head.size > 0)
            {
                char text[64];
                std::snprintf(text, sizeof(text), "  %d%%",
                              static_cast<int>(100.0 * static_cast<double>(head.done) /
                                               static_cast<double>(head.size)));
                status += text;
                if (queue_.rate > 1.0)
                {
                    std::snprintf(text, sizeof(text), "  \u00b7  %.0f MB/s", queue_.rate / 1e6);
                    status += text;
                }
            }
            if (queue_.entries.size() > 1)
                status += "  \u00b7  " + std::to_string(queue_.entries.size() - 1) + " more queued";
            pulsing = true;
        }
        else if (queue_.state == QueueState::paused)
        {
            // The hints below show Triangle opening the queue (a "\u25b3" here
            // isn't in the font: it showed as "?").
            status = "Queue paused  \u00b7  " + std::to_string(queue_.entries.size()) +
                     (queue_.entries.size() == 1 ? " game" : " games");
        }
        else if (queue_.state == QueueState::blocked)
        {
            status = queue_.message;
            status_ink = theme.warning;
        }
        else if (queue_.state == QueueState::failed)
        {
            status = "Copying stopped: " + queue_.message;
            status_ink = theme.danger;
        }
        else if (removal_.busy())
        {
            status = "Deleting " + removal_.title;
            pulsing = true;
        }
        else if (nas_.active() && !loading_ && source_ != Source::loading)
        {
            status = progress_text_.empty() ? "Updating" : progress_text_;
            if (progress_total_ > 1)
                status += "  (" + std::to_string(progress_done_ + 1) + " of " +
                          std::to_string(progress_total_) + ")";
            pulsing = true;
        }
        if (!status.empty())
        {
            // At the right edge, under the chips, with a pulsing dot before it
            // while work goes on.
            constexpr float kRight = 1824.0f;
            const std::string shown_status = ui::fit_label(paint, status, 21.0f, 900.0f);
            const float width = paint.body_width(shown_status, 21.0f);
            paint.body(shown_status, kRight, 196, 21, status_ink, gfx::Align::right);
            if (pulsing)
            {
                const float glow = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(clock_ * 4.0f));
                list.circle(kRight - width - 16.0f, 189.0f, 5.0f, theme.primary.with_alpha(glow));
            }
        }

        ui::Canvas canvas{list, context_.fonts, glass, clock_};
        tabs_.draw(canvas);

        // Connection and destination, as frosted chips on the right.
        constexpr float kChipsRight = 1824.0f;
        const Rect storage{kChipsRight - 372.0f, 102.0f, 372.0f, 52.0f};
        // The chip names where copies go now.
        char free_text[32];
        const Destination *place = destination();
        const bool internal = destination_ == kInternalGames;
        if (place != nullptr && place->total > 0)
            std::snprintf(free_text, sizeof(free_text), "%.1f GB free",
                          static_cast<double>(place->free) / 1e9);
        else if (internal && space_.known)
            std::snprintf(free_text, sizeof(free_text), "%.1f GB free",
                          static_cast<double>(space_.free) / 1e9);
        else
            std::snprintf(free_text, sizeof(free_text),
                          space_.asked ? "Free space unknown" : "Checking free space");
        const std::string chip_name = internal ? "Internal" : destination_name(destination_);
        draw_chip(list, paint, glass, storage, chip_name.c_str(), free_text, nullptr);
        // The server, with the last test's result as a dot (none: untested).
        const Connection &nas = connection_.connection();
        const std::string server =
            nas.configured() ? ui::fit_label(paint, nas.host, 21.0f, 260.0f) : "Set up NAS";
        const ProbeResult::State tested = connection_.test_result().state;
        const Color dot = tested == ProbeResult::State::ok       ? theme.success
                          : tested == ProbeResult::State::failed ? theme.danger
                                                                 : theme.primary;
        const bool show_dot = nas.configured() && tested != ProbeResult::State::idle;
        const float server_w = paint.label_width(server, 21.0f) + (show_dot ? 72.0f : 48.0f);
        draw_chip(list, paint, glass, {storage.x - 16.0f - server_w, 102.0f, server_w, 52.0f},
                  server.c_str(), "", show_dot ? &dot : nullptr);

        // ---- covers, or why there are none ----
        if (shown_.empty())
            draw_empty(list, paint);
        else
            grid_.draw(canvas);

        // ---- hints ----
        const ui::Hint hints[] = {{ui::Button::dpad, "Move"},
                                  {ui::Button::l2, "Filter", ui::Button::r2},
                                  {ui::Button::triangle, queue_hint_.c_str()},
                                  {ui::Button::options, "Settings"},
                                  {ui::Button::cross, "Open"}};
        ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints, 5, 1824, true);

        list.pop_transform();
        list.pop_opacity();
    }

    void draw_empty(gfx::DrawList &list, ui::Painter &paint) const
    {
        std::string headline;
        std::string detail;
        bool spinner = false;
        switch (source_)
        {
        case Source::unset:
            headline = "Set up your NAS";
            detail = "Press Options to enter the NAS's address.";
            break;
        case Source::loading:
            headline = "Reading your NAS";
            detail = connection_.connection().display_url();
            spinner = true;
            break;
        case Source::failed:
            headline = failure_.headline;
            detail = failure_.detail + " Press Options to check the settings.";
            break;
        case Source::demo:
        case Source::ready:
            if (games_.empty())
            {
                headline = "No games here yet";
                detail = "Put game folders or game images (.exfat, .ffpfsc) in " +
                         connection_.connection().folder + " on your NAS.";
            }
            else if (filter_ == kInstalled)
            {
                headline = "Nothing from your NAS is on this PS5";
                detail = "Press L2 or R2 to see the other games.";
            }
            else
            {
                headline = "Everything is already on this PS5";
                detail = "Press L2 or R2 to see the other games.";
            }
            break;
        }
        const ui::Theme &theme = acrylic();
        const float cy = kGrid.y + 220.0f;
        if (spinner)
        {
            // Three dots that pulse in turn.
            for (int i = 0; i < 3; ++i)
            {
                const float phase = clock_ * 3.0f - static_cast<float>(i) * 0.6f;
                const float glow = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(phase));
                list.circle(936.0f + static_cast<float>(i) * 24.0f, cy - 90.0f, 7.0f,
                            theme.primary.with_alpha(glow));
            }
        }
        paint.heading(ui::fit_label(paint, headline, 36.0f, 1500.0f), 960.0f, cy, 36,
                      paint.page_text(), gfx::Align::center);
        paint.body(ui::fit_label(paint, detail, 24.0f, 1500.0f), 960.0f, cy + 50.0f, 24,
                   paint.page_text_muted(), gfx::Align::center);
    }

    void draw_chip(gfx::DrawList &list, ui::Painter &paint, std::uint32_t glass, const Rect &r,
                   const char *label, const char *value, const Color *dot) const
    {
        const ui::Theme &theme = acrylic();
        if (glass != 0)
            list.glass(glass, r, r.h * 0.5f, theme.surface.with_alpha(0.55f));
        else
            list.rounded_rect(r, r.h * 0.5f, theme.surface);
        list.bordered_rect(r, r.h * 0.5f, Color::rgb(0x000000, 0.0f), 1.0f,
                           theme.light.with_alpha(0.35f));
        float x = r.x + 24.0f;
        if (dot != nullptr)
        {
            list.circle(x + 6.0f, r.cy(), 6.0f, *dot);
            x += 24.0f;
        }
        paint.label(label, x, r.cy() + 8.0f, 21, theme.text);
        if (value[0] != '\0')
            paint.body(value, r.x + r.w - 24.0f, r.cy() + 8.0f, 21, theme.text_muted,
                       gfx::Align::right);
    }

    // ---- game page ----
    const Game &page_entry() const
    {
        return games_[static_cast<std::size_t>(page_game())];
    }

    void draw_key_art(gfx::DrawList &list, float t) const
    {
        if (page_game() >= static_cast<int>(games_.size()))
            return;
        const Rect screen{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight};
        // The game's key art (sce_sys/pic0.png, 1920 x 1080) when it has
        // arrived; until then, or without one, the waves show through.
        const float art = t * art_fade_.value;
        if (key_art_ != 0 && key_art_for_ == page_game() && art > 0.001f)
        {
            const float zoom = 1.0f + 0.04f * (1.0f - art_fade_.value); // settles as it fades in
            list.push_transform(zoom, 960.0f, 540.0f, 0.0f, 0.0f);
            list.image(key_art_, screen, gfx::kFullUv, kWhite.with_alpha(art));
            list.pop_transform();
        }
        // Darken the left and the bottom so text and panels read over any art.
        list.gradient_rect_h(screen, 0.0f, kBlack.with_alpha(0.78f * t), kBlack.with_alpha(0.1f * t));
        list.gradient_rect({0.0f, 540.0f, gfx::kVirtualWidth, 540.0f}, 0.0f,
                           kBlack.with_alpha(0.0f), kBlack.with_alpha(0.7f * t));
    }

    void draw_page(gfx::DrawList &list, std::uint32_t glass, float t) const
    {
        if (page_game() >= static_cast<int>(games_.size()))
            return;
        const ui::Theme &theme = acrylic();
        const Game &game = page_entry();
        ui::Painter paint(list, context_.fonts, theme, glass);
        const float content = tween::smoothstep(tween::clamp01((t - 0.5f) / 0.5f));
        const float rise = 28.0f * (1.0f - content);

        // ---- title and details ----
        list.push_opacity(content);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, rise);
        paint.label(game.label, kMargin, 532, 22, theme.primary);
        paint.heading(ui::fit_label(paint, game.title, 76.0f, 1080.0f), kMargin - 3, 610, 76,
                      kWhite);
        std::string line;
        const auto add = [&line](const std::string &part)
        {
            if (part.empty())
                return;
            if (!line.empty())
                line += "   ·   ";
            line += part;
        };
        if (game.size_gb > 0.0)
            add(gigabytes(game.size_gb));
        add(game.format);
        add(game.title_id);
        add(game.version.empty() ? "" : "v" + game.version);
        paint.body(line, kMargin, 662, 26, kWhite.with_alpha(0.78f));
        if (game.on_ps5)
        {
            const Rect chip{kMargin, 700.0f, 170.0f, 44.0f};
            list.rounded_rect(chip, 22.0f, theme.success.with_alpha(0.22f));
            list.circle(chip.x + 22.0f, chip.cy(), 6.0f, theme.success);
            paint.label("On PS5", chip.x + 40.0f, chip.cy() + 8.0f, 20, kWhite);
        }
        const float blurb_y = game.on_ps5 ? 800.0f : 730.0f;
        paint.body(ui::fit_label(paint, game.blurb, 24.0f, 1080.0f), kMargin, blurb_y, 24,
                   kWhite.with_alpha(0.62f));
        if (!game.note.empty())
            paint.body(ui::fit_label(paint, "Couldn't read this game's details: " + game.note,
                                     22.0f, 1080.0f),
                       kMargin, blurb_y + 40.0f, 22, theme.danger);
        list.pop_transform();
        list.pop_opacity();

        draw_actions(list, paint, content, game);

        // ---- the shared element: the cover travels from its cell ----
        const Rect cover = mix_rect(page_from_, kPageCover, tween::cubic_out(t));
        const float radius = tween::lerp(kCoverRadius, 18.0f, t);
        list.shadow({cover.x, cover.y + 18.0f * t, cover.w, cover.h}, radius, 48,
                    kBlack.with_alpha(0.5f * t));
        if (game.cover != 0)
        {
            list.image(game.cover, cover, game.uv, kWhite, radius);
        }
        else
        {
            // No cover: the card's own gradient, with the title's first letter.
            list.gradient_rect(cover, radius, game.mid, game.dark);
            // The first letter or digit ("[PPSA01234] Game" gives "P").
            std::string initial = "?";
            for (const char c : game.title)
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                {
                    initial.assign(1, c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c);
                    break;
                }
            paint.heading(initial, cover.cx(), cover.cy() + cover.h * 0.12f, cover.h * 0.36f,
                          kWhite.with_alpha(0.85f), gfx::Align::center);
        }
        list.bordered_rect(cover, radius, Color::rgb(0x000000, 0.0f), 1.5f,
                           kWhite.with_alpha(0.18f * t));

        // ---- hints ----
        list.push_opacity(content);
        const ui::Hint hints[] = {
            {ui::Button::dpad, "Move"}, {ui::Button::cross, "Select"}, {ui::Button::circle, "Back"}};
        ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints, 3, 1824, true);
        list.pop_opacity();
    }

    void draw_actions(gfx::DrawList &list, ui::Painter &paint, float content,
                      const Game &game) const
    {
        const ui::Theme &theme = acrylic();
        list.push_opacity(content);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 40.0f * (1.0f - content));
        Rect panel = kPanel;
        panel.x += ui::shake(refusal_.value, clock_, 8.0f);
        paint.panel(panel);

        const float x = panel.x + 40.0f;
        const float w = panel.w - 80.0f;
        const int at = queued_at(game);
        const bool removing = removal_is(game);
        std::string label;
        std::string heading;
        std::string text;
        Color text_ink = theme.text_muted;
        float progress = -1.0f; // 0..1 shows a bar
        Color bar = theme.primary;
        const CopyCheck check = copy_check(game);
        bool show_diagnosis = false;

        if (at == 0)
        {
            // The front of the queue.
            const QueueEntry &item = queue_.entries.front();
            heading = destination_name(item.destination);
            switch (queue_.state)
            {
            case QueueState::running:
                label = "Copying to";
                if (!queue_.detail.empty())
                    text = queue_.detail;
                else if (queue_.phase == TransferState::preparing)
                    text = "Listing the game's files";
                else if (queue_.phase == TransferState::finishing)
                    text = "Adding the game to the home screen";
                else
                    text = copy_progress(item, queue_.rate);
                break;
            case QueueState::blocked:
                label = "Waiting for space on";
                text = queue_.message;
                text_ink = theme.warning;
                bar = theme.warning;
                break;
            case QueueState::failed:
                label = "Copy stopped";
                text = queue_.message;
                text_ink = theme.danger;
                bar = theme.danger;
                break;
            default:
                label = "Paused";
                text = item.started ? copy_progress(item, 0.0) + " copied" : "Not started yet";
                if (!queue_.message.empty())
                    text = queue_.message + "  \u00b7  " + text;
                break;
            }
            if (item.started && item.size > 0)
                progress = static_cast<float>(static_cast<double>(item.done) /
                                              static_cast<double>(item.size));
        }
        else if (at > 0)
        {
            const QueueEntry &item = queue_.entries[static_cast<std::size_t>(at)];
            label = "In the queue for";
            heading = destination_name(item.destination);
            char line[160];
            std::snprintf(line, sizeof(line), "Number %d in the queue  \u00b7  %.1f GB", at + 1,
                          static_cast<double>(item.size) / 1e9);
            text = line;
        }
        else if (game.from_nas && std::find(queue_.discarding.begin(), queue_.discarding.end(),
                                            game.source.entry) != queue_.discarding.end())
        {
            label = "Removed from the queue";
            heading = game.title;
            text = "Deleting the files copied so far";
            const auto done = queue_.discarded.find(game.source.entry);
            progress = done != queue_.discarded.end() ? done->second : 0.0f;
        }
        else if (removing &&
                 (removal_.state == TransferState::deleting ||
                  removal_.state == TransferState::failed ||
                  (removal_.state == TransferState::deleted && game.on_ps5)))
        {
            label = removal_.state == TransferState::failed    ? "Couldn't delete"
                    : removal_.state == TransferState::deleted ? "Deleted"
                                                               : "Deleting";
            heading = game.title;
            text = removal_.message;
            text_ink = removal_.state == TransferState::failed ? theme.danger : theme.text_muted;
            if (removal_.state == TransferState::deleting && removal_.total_bytes > 0)
                progress = static_cast<float>(static_cast<double>(removal_.done_bytes) /
                                              static_cast<double>(removal_.total_bytes));
        }
        else if (game.on_ps5)
        {
            const SmpGame *smp = ps5_game(game);
            label = "Installed";
            heading = smp != nullptr && smp->source_type != "pkg"
                          ? "On " + destination_name(smp->path)
                          : "On this PS5";
            text = game.size_gb > 0.0 ? "Uses " + gigabytes(game.size_gb) : "";
        }
        else if (game.from_nas && queue_.waiting(game.source.entry) != nullptr)
        {
            // ShadowMountPlus counts NASBridge as a running game and holds
            // its scans until NASBridge closes.
            label = "Copied to";
            heading = destination_name(queue_.waiting(game.source.entry)->destination);
            text = "Close NASBridge to finish adding the game to the home screen";
            progress = 1.0f;
        }
        else if (game.from_nas && queue_.finished_entry == game.source.entry)
        {
            label = "Copied to";
            heading = destination_name(queue_.finished_destination);
            text = "ShadowMountPlus is adding the game to the home screen";
            progress = 1.0f;
        }
        else
        {
            label = "Copy to";
            heading = destination_name(destination_);
            const double free_gb = destination_free_gb();
            const double queued_gb = static_cast<double>(queue_.queued_for(destination_)) / 1e9;
            char free_text[160];
            switch (check)
            {
            case CopyCheck::fits:
                if (queued_gb > 0.0)
                    std::snprintf(free_text, sizeof(free_text),
                                  "%.1f GB free  \u00b7  %.1f GB queued  \u00b7  %.1f GB left after",
                                  free_gb, queued_gb, free_gb - queued_gb - game.size_gb);
                else
                    std::snprintf(free_text, sizeof(free_text),
                                  "%.1f GB free  \u00b7  %.1f GB left after copying", free_gb,
                                  free_gb - game.size_gb);
                break;
            case CopyCheck::too_big:
                if (queued_gb > 0.0)
                    std::snprintf(free_text, sizeof(free_text),
                                  "Needs %.1f GB: %.1f GB free, %.1f GB of it queued", game.size_gb,
                                  free_gb, queued_gb);
                else
                    std::snprintf(free_text, sizeof(free_text), "Needs %.1f GB, only %.1f GB free",
                                  game.size_gb, free_gb);
                text_ink = theme.danger;
                break;
            case CopyCheck::size_unknown:
                std::snprintf(free_text, sizeof(free_text),
                              "%.1f GB free  \u00b7  measuring this game", free_gb);
                break;
            case CopyCheck::space_unknown:
                std::snprintf(free_text, sizeof(free_text), "%s",
                              space_.asked ? "Free space unknown" : "Checking free space");
                show_diagnosis = true;
                break;
            case CopyCheck::unavailable:
                std::snprintf(free_text, sizeof(free_text), "Not connected  \u00b7  %s",
                              destination_.c_str());
                text_ink = theme.danger;
                break;
            case CopyCheck::not_writable:
            {
                const Destination *place = destination();
                std::snprintf(free_text, sizeof(free_text), "NASBridge can't write here (%s)",
                              place != nullptr ? place->why.c_str() : "");
                text_ink = theme.danger;
                break;
            }
            case CopyCheck::checking:
                std::snprintf(free_text, sizeof(free_text),
                              "%.1f GB free  \u00b7  checking the drive", free_gb);
                break;
            }
            text = free_text;
        }
        // A refusal's reason replaces the line for a few seconds.
        if (notice_age_ < kNoticeSeconds && !notice_.empty())
        {
            text = notice_;
            text_ink = theme.danger;
        }

        paint.label(label, x, panel.y + 66, 20, theme.text_muted);
        paint.heading(ui::fit_label(paint, heading, 34.0f, w), x, panel.y + 110, 34);
        if (!text.empty())
            paint.body(ui::fit_label(paint, text, 23.0f, w), x, panel.y + 150, 23, text_ink);
        if (progress >= 0.0f)
        {
            const Rect track{x, panel.y + 170.0f, w, 8.0f};
            list.rounded_rect(track, 4.0f, theme.text_muted.with_alpha(0.22f));
            const float fill = std::clamp(progress, 0.0f, 1.0f) * w;
            if (fill > 1.0f)
                list.rounded_rect({x, track.y, std::max(fill, 8.0f), track.h}, 4.0f,
                                  bar);
        }
        std::vector<std::string> lines;
        if (show_diagnosis)
        {
            std::size_t start = 0;
            while (start < space_.how.size())
            {
                std::size_t end = space_.how.find(" | ", start);
                if (end == std::string::npos)
                    end = space_.how.size();
                lines.push_back(space_.how.substr(start, end - start));
                start = end + 3;
            }
        }
        if (debug_info())
        {
            // The drives as NASBridge sees them, and ShadowMountPlus's mounts.
            std::string drives = space_.drives_known ? "Drives:" : "Drives (not known):";
            for (const std::string &drive : space_.drives)
                drives += " " + drive;
            if (space_.drives.empty())
                drives += " none";
            lines.push_back(drives);
            for (const std::string &line : space_.mount_lines)
                lines.push_back(line);
            for (const Destination &place : space_.destinations)
                if (!place.why.empty())
                    lines.push_back("Can't copy to " + place.path + " (" + place.why + ")");
        }
        float line_y = panel.y + panel.h + 30.0f;
        for (const std::string &line : lines)
        {
            paint.body(ui::fit_label(paint, line, 17.0f, panel.w), panel.x, line_y, 17,
                       theme.text_muted);
            line_y += 24.0f;
        }

        PageAction actions[kActions];
        const int count = page_actions(game, actions);
        for (int i = 0; i < count; ++i)
        {
            const Rect button{x, panel.y + 206.0f + static_cast<float>(i) * 92.0f, w, 72.0f};
            ui::Look look;
            look.focus = action_focus_[i].value;
            look.press = i == action_ ? press_.value : 0.0f;
            look.disabled = !actions[i].enabled;
            paint.button(button, actions[i].label,
                         actions[i].primary ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
                         look);
        }
        list.pop_transform();
        list.pop_opacity();
    }

    app::Context &context_;
    ui::GridView grid_;
    ui::TabBar tabs_;
    Filter filter_ = kAll;
    std::vector<Game> games_;
    std::vector<int> shown_; // indices into games_, in grid order
    Source source_ = Source::unset;
    ProbeResult failure_;
    StorageWatcher storage_{kSpaceSeconds}; // asks for the free space on its own thread
    StorageSpace space_;                    // its latest answer, taken each frame
    LibraryLoader nas_;
    LibraryLoader art_;          // one game's key art, fetched when its page opens
    std::uint32_t key_art_ = 0;  // its texture
    int key_art_for_ = -1;       // ... and which game it belongs to
    tween::Spring art_fade_;
    int revision_ = -1; // the connection settings the games were read with
    ConnectionPage connection_{context_.fonts};
    SettingsPage settings_screen_{context_.fonts, connection_}; // after connection_: it uses it
    bool settings_open_ = false;
    tween::Spring settings_;
    ui::LoadingScreen loader_;
    bool loading_ = true;       // the loading screen is up
    float loading_age_ = 0.0f;
    bool has_saved_ = false;    // the library came from the cache
    bool ready_ = false;        // the NAS check and its pictures are done
    bool offline_ = false;      // the NAS couldn't be reached: the saved library shows
    int progress_done_ = 0;     // the reader's latest progress
    int progress_total_ = 0;
    std::string progress_text_;
    ui::SpringColor tint_[4];
    float age_ = 0.0f;
    float clock_ = 0.0f;

    bool page_open_ = false;
    int page_game_ = 0;
    Rect page_from_{};
    tween::Spring page_;
    int action_ = 0;
    tween::Spring action_focus_[kActions];
    ui::Select places_;                     // where copies go (opens from "Destination")
    std::vector<std::string> place_paths_;  // the list's folders, in its order
    std::string places_shown_;              // places_signature() when the list was filled
    std::string destination_;               // the chosen folder
    std::string drive_resumed_;             // the drive the queue was resumed for (follow_drives)
    Transfers transfers_;                   // the copy or delete, on its own thread
    QueueStatus queue_;                     // the copy queue, taken each frame
    TransferStatus removal_;                // a delete from the PS5, taken each frame
    std::string queue_mark_;                // what the queue looked like (to notice changes)
    QueuePanel queue_panel_{context_.fonts, transfers_}; // opens with Triangle
    std::string queue_hint_ = "Queue";      // "Queue (3)"
    ui::Dialog confirm_;                    // "Delete ...?"
    Command confirm_command_ = Command::none;
    std::string notice_;                    // why the last press was refused
    float notice_age_ = kNoticeSeconds;
    float awake_age_ = kAwakeSeconds;       // since the PS5's idle timer was last reset
    ui::Pulse press_;
    ui::Pulse refusal_;
};

} // namespace

std::unique_ptr<app::Concept> make_library(app::Context &context)
{
    return std::make_unique<Library>(context);
}

} // namespace hui::concepts
