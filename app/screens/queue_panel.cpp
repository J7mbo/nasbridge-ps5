// NASBridge - The copy queue, as a panel that slides in over the library.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/queue_panel.hpp"

#include "concepts/app_settings.hpp"
#include "concepts/library_cache.hpp"

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

constexpr float kPanelW = 700.0f;
constexpr float kPanelY = 56.0f;
constexpr float kPanelH = 968.0f;
constexpr float kPanelRight = 1864.0f;
constexpr float kRadius = 36.0f;
constexpr float kListTop = 150.0f;  // from the panel's top
constexpr float kFooter = 312.0f;   // room under the list: tiles, hints
constexpr float kTileH = 196.0f;
constexpr float kSampleSeconds = 0.25f; // the graphs' step
constexpr int kSamples = 120;           // half a minute of them
constexpr float kRowH = 108.0f;
constexpr float kCover = 76.0f;
constexpr float kNoticeSeconds = 4.0f;
const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

std::string gb(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

// "14 min left"
std::string time_left(std::uint64_t bytes, double rate)
{
    if (rate <= 1.0)
        return "";
    const double seconds = static_cast<double>(bytes) / rate;
    char text[40];
    if (seconds < 60.0)
        std::snprintf(text, sizeof(text), "under 1 min left");
    else if (seconds < 3600.0)
        std::snprintf(text, sizeof(text), "%d min left", static_cast<int>(seconds / 60.0 + 0.5));
    else
        std::snprintf(text, sizeof(text), "%d h %02d min left", static_cast<int>(seconds / 3600.0),
                      static_cast<int>(seconds / 60.0) % 60);
    return text;
}

// Parts of a line joined with " · ", leaving out the least important (the
// highest `drop`; 0 never goes) until it fits `width`, rather than cutting
// the end off. Order on screen is kept.
struct Part
{
    std::string text;
    int drop = 0;
};

std::string fit_parts(const ui::Painter &paint, std::vector<Part> parts, float size, float width)
{
    const auto joined = [&parts]
    {
        std::string line;
        for (const Part &part : parts)
            if (!part.text.empty())
                line += (line.empty() ? "" : "  \u00b7  ") + part.text;
        return line;
    };
    std::string line = joined();
    while (paint.body_width(line, size) > width)
    {
        int worst = 0;
        std::size_t at = parts.size();
        for (std::size_t i = 0; i < parts.size(); ++i)
            if (!parts[i].text.empty() && parts[i].drop > worst)
            {
                worst = parts[i].drop;
                at = i;
            }
        if (at == parts.size())
            break; // only what must stay: shortened below
        parts[at].text.clear();
        line = joined();
    }
    return ui::fit_body(paint, line, size, width);
}

std::string grouped(int n)
{
    std::string out = std::to_string(n);
    for (int cut = static_cast<int>(out.size()) - 3; cut > 0; cut -= 3)
        out.insert(static_cast<std::size_t>(cut), ",");
    return out;
}

// A tile's graph: measured from zero (so a wobble of a few MB/s stays a
// wobble), filled beneath, a dot on the newest value. `low_top` is the least
// the top of the graph stands for, so a quiet start isn't drawn as a peak.
void draw_graph(gfx::DrawList &list, const Rect &area, const std::vector<float> &values,
                float low_top, Color ink)
{
    list.rounded_rect({area.x, area.y + area.h - 1.5f, area.w, 1.5f}, 0.0f,
                      kWhite.with_alpha(0.14f));
    if (values.size() < 2)
        return;
    float top = low_top;
    for (const float v : values)
        top = std::max(top, v);
    top *= 1.1f;
    const float margin = 5.0f;
    const auto px = [&](std::size_t i)
    {
        return area.x + (area.w - margin) * static_cast<float>(i) /
                            static_cast<float>(kSamples - 1) +
               (area.w - margin) * static_cast<float>(kSamples - values.size()) /
                   static_cast<float>(kSamples - 1);
    };
    const auto py = [&](std::size_t i)
    { return area.y + area.h - 1.5f - (area.h - margin - 1.5f) * values[i] / top; };
    const float floor_y = area.y + area.h - 1.5f;
    for (std::size_t i = 0; i + 1 < values.size(); ++i)
    {
        // The fill, a column at a time, then the line over it.
        const float x0 = px(i), x1 = px(i + 1);
        const float y0 = py(i), y1 = py(i + 1);
        const float column[8] = {x0, y0, x1, y1, x1, floor_y, x0, floor_y};
        list.polygon(column, 4, ink.with_alpha(0.16f));
    }
    for (std::size_t i = 0; i + 1 < values.size(); ++i)
        list.line(px(i), py(i), px(i + 1), py(i + 1), 3.0f, ink);
    list.circle(px(values.size() - 1), py(values.size() - 1), 5.0f, ink);
}

} // namespace

QueuePanel::QueuePanel(const ui::Fonts &fonts, Transfers &transfers)
    : fonts_(fonts), transfers_(transfers)
{
    confirm_.style.theme = acrylic();
    spinner_.style.theme = acrylic();
    spinner_.style.kind = ui::SpinnerKind::arc;
    spinner_.style.thickness = 5.0f;
    for (ui::StatTile *tile : {&speed_tile_, &links_tile_})
    {
        tile->style.theme = acrylic();
        tile->style.show_delta = false;
        tile->style.label_size = 18.0f;
        tile->style.value_size = 40.0f;
        tile->style.spark_height = 64.0f;
        tile->style.padding = 20.0f;
    }
    speed_tile_.label = "Speed";
    speed_tile_.style.format = ui::CounterFormat::decimal;
    speed_tile_.style.decimals = 1;
    speed_tile_.style.suffix = " MB/s";
    links_tile_.label = "Connections";
    // The graphs are drawn here, from zero, rather than scaled to fit.
    speed_tile_.sparkline = [this](ui::Canvas &canvas, const Rect &area)
    { draw_graph(canvas.list, area, speeds_, 10.0f, gfx::mix(acrylic().primary, kWhite, 0.3f)); };
    links_tile_.sparkline = [this](ui::Canvas &canvas, const Rect &area)
    { draw_graph(canvas.list, area, links_, 4.0f, gfx::mix(acrylic().primary, kWhite, 0.3f)); };
}

void QueuePanel::open(app::Feedback &feedback)
{
    open_ = true;
    lifted_ = false;
    lift_.snap(0.0f);
    focus_ = 0;
    ring_.snap(0.0f);
    scroll_.position.snap(0.0f);
    notice_age_ = kNoticeSeconds;
    age_ = 0.0f;
    row_y_.clear();
    feedback.play(audio::Cue::modal_open);
}

void QueuePanel::refuse(app::Feedback &feedback, const std::string &why, float dy)
{
    nudge_y_ = dy;
    nudge_.trigger();
    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.7f);
    if (!why.empty())
    {
        notice_ = why;
        notice_age_ = 0.0f;
    }
}

void QueuePanel::reveal()
{
    const float view = kPanelH - kListTop - kFooter;
    const float start = static_cast<float>(focus_) * kRowH;
    scroll_.reveal(start, start + kRowH, view, kRowH * 0.5f,
                   static_cast<float>(queue_.entries.size()) * kRowH);
}

void QueuePanel::handle(const InputFrame &input, app::Feedback &feedback,
                        const Connection &connection)
{
    if (!open_)
        return;
    std::string why;
    if (confirm_.is_open())
    {
        if (confirm_.handle(input, feedback) == ui::Event::activated && confirm_.choice() == 1)
        {
            queue_ = transfers_.queue();
            const int at = queue_.find(confirm_entry_);
            if (at < 0 || !transfers_.remove(at, &why))
                refuse(feedback, why.empty() ? "That game left the queue already" : why);
            else
                queue_ = transfers_.queue();
        }
        return;
    }
    const int count = static_cast<int>(queue_.entries.size());
    if (input.is_pressed(Action::back))
    {
        if (lifted_)
        {
            lifted_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        open_ = false;
        feedback.play(audio::Cue::modal_close);
        return;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int delta = input.nav == Direction::down ? 1 : -1;
        if (lifted_)
        {
            // Carry the lifted game one place.
            if (transfers_.move(focus_, delta, &why))
            {
                focus_ += delta;
                queue_ = transfers_.queue();
                reveal();
                feedback.play(audio::Cue::focus, delta > 0 ? 0.94f : 1.06f);
            }
            else if (!input.nav_repeat)
            {
                refuse(feedback, focus_ + delta < 0 || focus_ + delta >= count ? "" : why,
                       static_cast<float>(delta));
            }
            return;
        }
        const int next = focus_ + delta;
        if (next >= 0 && next < count)
        {
            focus_ = next;
            reveal();
            feedback.play(audio::Cue::focus,
                          1.04f - 0.08f * static_cast<float>(focus_) /
                                      std::max(1.0f, static_cast<float>(count)));
        }
        else if (!input.nav_repeat)
        {
            refuse(feedback, "", static_cast<float>(delta));
        }
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        if (count == 0)
            return refuse(feedback, "");
        if (lifted_)
        {
            lifted_ = false;
            feedback.play(audio::Cue::select, 0.9f);
            return;
        }
        if (queue_.running() && focus_ == 0)
            return refuse(feedback, "The game being copied stays first. Pause the queue to move that game.");
        lifted_ = true;
        feedback.play(audio::Cue::select, 1.1f);
        return;
    }
    if (input.is_pressed(Action::west))
    {
        // Pause or resume the queue.
        if (queue_.running())
        {
            if (transfers_.pause())
                feedback.play(audio::Cue::select);
        }
        else if (count == 0)
        {
            refuse(feedback, "Nothing is queued");
        }
        else if (transfers_.resume(connection, &why))
        {
            feedback.play(audio::Cue::select);
        }
        else
        {
            refuse(feedback, why);
        }
        queue_ = transfers_.queue();
        return;
    }
    if (input.is_pressed(Action::north))
    {
        // Remove the focused game.
        if (count == 0)
            return refuse(feedback, "");
        lifted_ = false;
        const QueueEntry &item = queue_.entries[static_cast<std::size_t>(focus_)];
        if (queue_.running() && focus_ == 0)
            return refuse(feedback, "Pause the queue first, then remove the game");
        if (!item.started)
        {
            // Nothing on the drive yet: it just leaves the queue.
            if (!transfers_.remove(focus_, &why))
                return refuse(feedback, why);
            feedback.play(audio::Cue::back);
            queue_ = transfers_.queue();
            return;
        }
        confirm_entry_ = item.entry;
        ui::DialogContent content;
        content.icon = ui::StatusKind::danger;
        content.title = "Remove " + item.title + " from the queue?";
        content.body = "The " + gb(item.done) + " copied so far will be deleted from " +
                       destination_name(item.destination) + ". The game on your NAS isn't touched.";
        content.buttons = {{"Cancel"}, {"Remove", ui::ButtonKind::primary, true}};
        confirm_.open(std::move(content), feedback);
    }
}

void QueuePanel::update(float dt, const QueueStatus &queue)
{
    queue_ = queue;
    age_ += dt;
    notice_age_ += dt;
    shown_.target = open_ ? 1.0f : 0.0f;
    shown_.update(dt, open_ ? 13.0f : 20.0f);
    confirm_.update(dt);

    // The live figures: the speed only counts while data is coming in.
    const bool copying = queue_.running() && queue_.phase == TransferState::copying;
    const float speed = copying ? static_cast<float>(queue_.rate / 1e6) : 0.0f;
    const float links = copying ? static_cast<float>(queue_.connections) : 0.0f;
    sample_clock_ += dt;
    if (sample_clock_ >= kSampleSeconds)
    {
        sample_clock_ = std::fmod(sample_clock_, kSampleSeconds);
        speeds_.push_back(speed);
        links_.push_back(links);
        if (static_cast<int>(speeds_.size()) > kSamples)
        {
            speeds_.erase(speeds_.begin());
            links_.erase(links_.begin());
        }
    }
    speed_tile_.set_value(speed);
    links_tile_.set_value(links);
    speed_tile_.update(dt);
    links_tile_.update(dt);
    spinner_.set_spinning(queue_.running());
    spinner_.update(dt);

    const int count = static_cast<int>(queue_.entries.size());
    if (focus_ >= count)
    {
        focus_ = std::max(0, count - 1);
        lifted_ = false;
    }
    // The game being copied can't be carried while the queue runs.
    if (lifted_ && queue_.running() && focus_ == 0)
        lifted_ = false;
    ring_.target = static_cast<float>(focus_);
    ring_.update(dt, 22.0f);
    lift_.target = lifted_ ? 1.0f : 0.0f;
    lift_.update(dt, 18.0f);
    scroll_.update(dt);
    nudge_.update(dt, 9.0f);
    // Each game's row glides to its place; new ones arrive where they belong.
    for (int i = 0; i < count; ++i)
    {
        const std::string &key = queue_.entries[static_cast<std::size_t>(i)].entry;
        const float y = static_cast<float>(i) * kRowH;
        auto found = row_y_.find(key);
        if (found == row_y_.end())
            row_y_[key].snap(y);
        else
        {
            found->second.target = y;
            found->second.update(dt, lifted_ && i == focus_ ? 26.0f : 16.0f);
        }
    }
    for (auto it = row_y_.begin(); it != row_y_.end();)
        it = queue_.find(it->first) < 0 ? row_y_.erase(it) : std::next(it);
}

void QueuePanel::draw(gfx::DrawList &list, std::uint32_t glass, float clock,
                      const QueueStatus &queue, const CoverOf &cover_of) const
{
    const float t = tween::clamp01(shown_.value);
    if (t <= 0.01f)
        return;
    const ui::Theme &theme = acrylic();
    ui::Painter paint(list, fonts_, theme, glass);
    const Color accent = gfx::mix(theme.primary, kWhite, 0.3f);

    // A veil over the library, then the panel sliding in from the right.
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0, 0.35f * t));
    const float slide = (1.0f - tween::cubic_out(t)) * (kPanelW + 120.0f);
    const Rect panel{kPanelRight - kPanelW + slide, kPanelY, kPanelW, kPanelH};
    list.push_opacity(tween::clamp01(t * 1.5f));
    list.shadow({panel.x, panel.y + 20.0f, panel.w, panel.h}, kRadius, 56, Color::rgb(0, 0.42f));
    list.glass(glass, panel, kRadius, kWhite);
    list.rounded_rect(panel, kRadius, Color::rgb(0x0b0d16, 0.55f));
    list.bordered_rect(panel, kRadius, kClear, 1.5f, kWhite.with_alpha(0.2f));

    const float x = panel.x + 36.0f;
    const float right = panel.x + panel.w - 36.0f;
    const int count = static_cast<int>(queue.entries.size());

    // ---- header: what's queued, and what the queue is doing ----
    paint.label("Queue", x, panel.y + 60.0f, 18, accent);
    std::uint64_t total = 0;
    for (const QueueEntry &item : queue.entries)
        total += item.size;
    if (count > 0)
        paint.body(std::to_string(count) + (count == 1 ? " game  ·  " : " games  ·  ") +
                       gb(total),
                   right, panel.y + 60.0f, 20, kWhite.with_alpha(0.6f), gfx::Align::right);

    std::string status;
    Color status_ink = kWhite.with_alpha(0.75f);
    switch (queue.state)
    {
    case QueueState::idle:
        status = "Nothing queued";
        break;
    case QueueState::running:
    {
        const QueueEntry &head = queue.entries.front();
        if (!queue.detail.empty())
            status = queue.detail;
        else if (queue.phase == TransferState::copying && head.size > 0)
        {
            // The file count of a folder game is on its own row below.
            char text[96];
            std::vector<Part> parts;
            parts.push_back({"Copying"});
            std::snprintf(text, sizeof(text), "%d%%",
                          static_cast<int>(100.0 * static_cast<double>(head.done) /
                                           static_cast<double>(head.size)));
            parts.push_back({text});
            if (queue.rate > 1.0)
            {
                std::snprintf(text, sizeof(text), "%.0f MB/s", queue.rate / 1e6);
                parts.push_back({text, 2});
            }
            // Time left for the whole queue at this speed.
            const std::string left = time_left(total > head.done ? total - head.done : 0,
                                               queue.rate);
            if (!left.empty())
                parts.push_back({"all done in " + left.substr(0, left.size() - 5), 1});
            status = fit_parts(paint, parts, 21.0f, panel.w - 72.0f);
        }
        else
            status = "Starting";
        break;
    }
    case QueueState::paused:
        status = queue.message.empty() ? "Paused" : "Paused  ·  " + queue.message;
        break;
    case QueueState::blocked:
        status = queue.message;
        status_ink = theme.warning;
        break;
    case QueueState::failed:
        status = "Stopped: " + queue.message;
        status_ink = theme.danger;
        break;
    }
    if (notice_age_ < kNoticeSeconds && !notice_.empty())
    {
        status = notice_;
        status_ink = theme.warning;
    }
    paint.body(ui::fit_body(paint, status, 21.0f, panel.w - 72.0f), x, panel.y + 104.0f, 21,
               status_ink);
    // The copy helper's diagnostic line, in the mono face (Settings > Debug).
    if (debug_info() && !queue.conn_report.empty())
        ui::text(list, fonts_.mono, queue.conn_report, x, panel.y + 124.0f, 15,
                 kWhite.with_alpha(0.5f));
    list.rounded_rect({x, panel.y + 134.0f, panel.w - 72.0f, 1.5f}, 0.75f,
                      kWhite.with_alpha(0.12f));

    // ---- the list ----
    const float top = panel.y + kListTop;
    const float view = panel.h - kListTop - kFooter;
    const float origin = top - scroll_.offset();
    if (count == 0)
    {
        // An empty queue says how to fill it.
        const float cx = panel.x + panel.w * 0.5f;
        const float cy = top + view * 0.38f;
        for (int i = 2; i >= 0; --i)
        {
            const float d = static_cast<float>(i) * 12.0f;
            list.rounded_rect({cx - 60.0f + d, cy - 44.0f - d, 120.0f - 2.0f * d, 88.0f}, 12.0f,
                              gfx::mix(theme.primary, Color::rgb(0x0b0d16), 0.5f + 0.15f * i)
                                  .with_alpha(0.8f));
        }
        paint.body("Nothing queued", cx, cy + 96.0f, 27, kWhite, gfx::Align::center);
        paint.body("Open a game and choose Download to PS5.", cx, cy + 136.0f, 21,
                   kWhite.with_alpha(0.6f), gfx::Align::center);
    }
    list.push_clip({panel.x, top - 6.0f, panel.w, view + 12.0f});

    const auto row_y = [&](const std::string &entry, int index)
    {
        const auto found = row_y_.find(entry);
        return origin + (found != row_y_.end() ? found->second.value
                                               : static_cast<float>(index) * kRowH);
    };
    // Rows fade at the list's edges rather than being cut.
    const auto edge = [&](float y)
    {
        return tween::clamp01((y - top + 30.0f) / 40.0f) *
               tween::clamp01((top + view - (y + kRowH) + 30.0f) / 40.0f);
    };

    // The highlight springs between rows; lifted, it glows and the row rises.
    if (count > 0 && !confirm_.visible())
    {
        Rect ring{panel.x + 16.0f, origin + ring_.value * kRowH + 4.0f, panel.w - 32.0f,
                  kRowH - 8.0f};
        ring.y += ui::shake(nudge_.value, clock, 7.0f, 9.0f) * (nudge_y_ == 0.0f ? 1.0f : nudge_y_);
        const float lifted = lift_.value;
        list.glow(ring, 20.0f, 14.0f + 10.0f * lifted, accent.with_alpha(0.22f + 0.25f * lifted));
        list.rounded_rect(ring, 20.0f, kWhite.with_alpha(0.14f + 0.06f * lifted));
        list.bordered_rect(ring, 20.0f, kClear, 2.5f + lifted, gfx::mix(kWhite, accent, lifted));
    }

    // Draw the lifted row last, so it rides over the others.
    std::vector<int> order;
    for (int i = 0; i < count; ++i)
        if (!(lifted_ && i == focus_))
            order.push_back(i);
    if (lifted_ && focus_ < count)
        order.push_back(focus_);
    for (const int i : order)
    {
        const QueueEntry &item = queue.entries[static_cast<std::size_t>(i)];
        const bool head = i == 0;
        const bool lifted = lifted_ && i == focus_;
        const float y = row_y(item.entry, i) - (lifted ? 6.0f * lift_.value : 0.0f);
        const float alpha = edge(y) * tween::clamp01((age_ - 0.04f * static_cast<float>(i)) / 0.3f);
        if (alpha <= 0.01f)
            continue;
        list.push_opacity(alpha);
        if (lifted)
            list.shadow({panel.x + 24.0f, y + 16.0f, panel.w - 48.0f, kRowH - 12.0f}, 20.0f, 30.0f,
                        Color::rgb(0, 0.45f * lift_.value));
        const float cy = y + kRowH * 0.5f;

        // The cover (or its colours).
        const Cover cover = cover_of ? cover_of(item.entry) : Cover{};
        const Rect art{x + 46.0f, cy - kCover * 0.5f, kCover, kCover};
        if (cover.texture != 0)
            list.image(cover.texture, art, cover.uv, kWhite, 12.0f);
        else
            list.gradient_rect(art, 12.0f, cover.top, cover.bottom);
        list.bordered_rect(art, 12.0f, kClear, 1.0f, kWhite.with_alpha(0.16f));

        // Its place: the Arc on the game being copied, a number on the rest.
        if (head && queue.running())
        {
            ui::Spinner arc = spinner_;
            arc.style.color = accent;
            arc.set_bounds({x - 4.0f, cy - 17.0f, 34.0f, 34.0f});
            ui::Canvas canvas{list, fonts_, glass, clock};
            arc.draw(canvas);
        }
        else if (head && (queue.state == QueueState::paused))
        {
            list.rounded_rect({x + 4.0f, cy - 11.0f, 6.0f, 22.0f}, 2.0f, kWhite.with_alpha(0.75f));
            list.rounded_rect({x + 16.0f, cy - 11.0f, 6.0f, 22.0f}, 2.0f, kWhite.with_alpha(0.75f));
        }
        else if (head && (queue.state == QueueState::blocked || queue.state == QueueState::failed))
        {
            const Color mark = queue.state == QueueState::blocked ? theme.warning : theme.danger;
            list.circle(x + 13.0f, cy, 13.0f, mark.with_alpha(0.9f));
            paint.body("!", x + 13.0f, cy + 8.0f, 22, Color::rgb(0x111111), gfx::Align::center);
        }
        else
        {
            char number[8];
            std::snprintf(number, sizeof(number), "%02d", i + 1);
            ui::text(list, fonts_.mono, number, x + 13.0f, cy + 7.0f, 20, kWhite.with_alpha(0.5f),
                     gfx::Align::center);
        }

        // Title, and what's happening to it.
        const float tx = art.x + kCover + 22.0f;
        const float tw = right - tx;
        paint.body(ui::fit_label(paint, item.title, 25.0f, tw), tx, cy - 8.0f, 25,
                   kWhite.with_alpha(i == focus_ ? 1.0f : 0.88f));
        // What doesn't fit goes, least useful first: where it's going, then
        // the time left (the status line has the queue's), never the size.
        std::vector<Part> parts;
        if (head && item.started && item.size > 0 && queue.state != QueueState::idle)
        {
            parts.push_back({gb(item.done) + " of " + gb(item.size)});
            // A folder game: which file (thousands of small ones copy far
            // slower than their size suggests).
            if (queue.files_total > 1)
                parts.push_back({grouped(std::min(queue.files_done + 1, queue.files_total)) + "/" +
                                     grouped(queue.files_total) + " files",
                                 1});
            if (queue.running() && queue.phase == TransferState::copying && queue.rate > 1.0)
                parts.push_back({time_left(item.size - item.done, queue.rate), 2});
        }
        else
        {
            parts.push_back({gb(item.size)});
        }
        parts.push_back({destination_name(item.destination), 3});
        paint.body(fit_parts(paint, parts, 19.0f, tw), tx, cy + 24.0f, 19,
                   kWhite.with_alpha(0.6f));
        if (item.started && item.size > 0)
        {
            // How far it got: a thin bar under the text.
            const Rect track{tx, cy + 36.0f, tw, 4.0f};
            const float fill = tween::clamp01(static_cast<float>(
                static_cast<double>(item.done) / static_cast<double>(item.size)));
            list.rounded_rect(track, 2.0f, kWhite.with_alpha(0.12f));
            if (fill > 0.0f)
                list.rounded_rect({tx, track.y, std::max(4.0f, tw * fill), 4.0f}, 2.0f,
                                  head && queue.state == QueueState::failed ? theme.danger
                                                                             : accent);
        }
        list.pop_opacity();
    }
    list.pop_clip();

    // A thin thumb says where in a long queue you are.
    const float content = static_cast<float>(count) * kRowH;
    if (content > view)
    {
        const Rect track{panel.x + panel.w - 12.0f, top + 4.0f, 3.0f, view - 8.0f};
        const float size = std::max(40.0f, track.h * view / content);
        const float at = tween::clamp01(scroll_.offset() / (content - view));
        list.rounded_rect(track, 1.5f, kWhite.with_alpha(0.07f));
        list.rounded_rect({track.x, track.y + (track.h - size) * at, 3.0f, size}, 1.5f,
                          kWhite.with_alpha(0.4f));
    }

    // ---- footer: the live speed and the connections bringing it ----
    const float fy = panel.y + panel.h - kFooter + 14.0f;
    list.rounded_rect({x, fy - 4.0f, panel.w - 72.0f, 1.5f}, 0.75f, kWhite.with_alpha(0.12f));
    {
        ui::Canvas canvas{list, fonts_, glass, clock};
        const float gap = 16.0f;
        const float width = panel.w - 72.0f;
        const float links_w = 200.0f;
        ui::StatTile speed = speed_tile_;
        speed.set_bounds({x, fy + 14.0f, width - links_w - gap, kTileH});
        speed.draw(canvas);
        ui::StatTile links = links_tile_;
        links.set_bounds({right - links_w, fy + 14.0f, links_w, kTileH});
        links.draw(canvas);
    }

    ui::HintLayout layout;
    layout.size = 32.0f;
    layout.text_size = 20.0f;
    layout.cy = panel.y + panel.h - 44.0f;
    layout.item_gap = 28.0f;
    const ui::Hint hints[] = {{ui::Button::cross, lifted_ ? "Put down" : "Move"},
                              {ui::Button::square, queue.running() ? "Pause" : "Resume"},
                              {ui::Button::triangle, "Remove"},
                              {ui::Button::circle, "Close"}};
    ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), hints, 4, right, true, layout);
    list.pop_opacity();

    if (confirm_.visible())
    {
        ui::Canvas canvas{list, fonts_, glass, clock};
        confirm_.draw(canvas);
    }
}

} // namespace hui::concepts
