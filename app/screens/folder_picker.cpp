// NASBridge - Choosing the games folder on the NAS.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/folder_picker.hpp"

#include "concepts/http.hpp"
#include "concepts/nfs.hpp"
#include "concepts/smb.hpp"
#include "concepts/nas_probe.hpp"

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>

#if defined(__PROSPERO__)
#include <pthread_np.h>
#include <sys/cpuset.h>
extern "C" int sceKernelGetCurrentCpu(void);
#endif

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;

// ---- the design language (after files.cpp, in NASBridge's Acrylic) ----

const Rect kPanel{520.0f, 132.0f, 880.0f, 816.0f};
constexpr float kPad = 44.0f;
constexpr float kListTop = 254.0f; // from the panel's top
constexpr float kRowH = 58.0f;
constexpr int kRows = 9;
constexpr float kListH = kRowH * static_cast<float>(kRows);
constexpr float kRadius = 10.0f;
constexpr float kIconX = 34.0f; // from the list's left edge
constexpr float kNameX = 70.0f;
constexpr float kNameSize = 25.0f;
const Color kFolderTint = Color::rgb(0x6fb2ff); // the File Browser's folder blue
const Color kGraphite = Color::rgb(0x14171c);
const Color kWhite = Color::rgb(0xffffff);

const ui::Theme &acrylic()
{
    for (const ui::Theme &theme : ui::themes())
        if (std::strcmp(theme.id, "acrylic") == 0)
            return theme;
    return ui::themes()[0];
}

// The File Browser's folder: a tab behind a solid front flap.
void draw_folder(gfx::DrawList &list, float cx, float cy, float s, float alpha = 1.0f)
{
    const float h = s * 0.5f;
    const Color back = gfx::mix(kFolderTint, kGraphite, 0.38f).with_alpha(alpha);
    list.rounded_rect({cx - h, cy - s * 0.42f, s * 0.46f, s * 0.3f}, s * 0.09f, back);
    list.rounded_rect({cx - h, cy - s * 0.3f, s, s * 0.72f}, s * 0.1f, back);
    list.gradient_rect({cx - h, cy - s * 0.19f, s, s * 0.61f}, s * 0.1f,
                       kFolderTint.with_alpha(alpha),
                       gfx::mix(kFolderTint, kGraphite, 0.2f).with_alpha(alpha));
}

void draw_chevron(gfx::DrawList &list, float cx, float cy, float s, Color color)
{
    list.line(cx - s * 0.3f, cy - s * 0.5f, cx + s * 0.3f, cy, 2.0f, color);
    list.line(cx + s * 0.3f, cy, cx - s * 0.3f, cy + s * 0.5f, 2.0f, color);
}

int compare_names(const std::string &a, const std::string &b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + 32) : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + 32) : b[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// "/games/PS5/" for any way of writing a folder.
std::string as_folder(std::string path)
{
    if (path.empty() || path.front() != '/')
        path.insert(path.begin(), '/');
    if (path.back() != '/')
        path += '/';
    return path;
}

std::string parent_of(const std::string &folder)
{
    if (folder.size() <= 1)
        return "/";
    const std::size_t slash = folder.rfind('/', folder.size() - 2);
    return slash == std::string::npos ? "/" : folder.substr(0, slash + 1);
}

std::string leaf_of(const std::string &folder)
{
    if (folder.size() <= 1)
        return "";
    const std::size_t slash = folder.rfind('/', folder.size() - 2);
    return folder.substr(slash + 1, folder.size() - slash - 2);
}

} // namespace

void draw_folder_icon(gfx::DrawList &list, float cx, float cy, float size, float alpha)
{
    draw_folder(list, cx, cy, size, alpha);
}

bool folders_from_listing(const std::string &body, const std::string &path,
                          std::vector<std::string> *names)
{
    names->clear();
    std::vector<DavEntry> entries;
    if (!parse_listing(body, path, &entries))
        return false;
    for (const DavEntry &entry : entries)
        if (entry.folder)
            names->push_back(entry.name);
    std::sort(names->begin(), names->end(),
              [](const std::string &a, const std::string &b) { return compare_names(a, b) < 0; });
    names->erase(std::unique(names->begin(), names->end()), names->end());
    return true;
}

// ---- the worker ----

struct FolderPicker::Shared
{
    std::mutex lock;
    struct Answer
    {
        std::string path;
        Listing listing;
    };
    std::vector<Answer> answers; // finished listings, not yet taken
    int ui_cpu = -1;
};

namespace
{

struct Ask
{
    std::shared_ptr<FolderPicker::Shared> shared;
    Connection connection;
    std::string path;
};

void *ask_main(void *argument)
{
    Ask *ask = static_cast<Ask *>(argument);
#if defined(__PROSPERO__)
    // Off the UI's core: console threads aren't time-sliced.
    if (ask->shared->ui_cpu >= 0)
    {
        cpuset_t allowed;
        CPU_ZERO(&allowed);
        if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) == 0 &&
            CPU_ISSET(ask->shared->ui_cpu, &allowed))
        {
            CPU_CLR(ask->shared->ui_cpu, &allowed);
            if (!CPU_EMPTY(&allowed))
                pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed);
        }
    }
#endif
    FolderPicker::Shared::Answer answer;
    answer.path = ask->path;
    if (ask->connection.type == ConnectionType::smb)
    {
        // At "/", the NAS's shares; below, the folders in them.
        SmbSession session(ask->connection);
        std::vector<SmbEntry> entries;
        SmbFailure failure;
        if (session.list(ask->path, &entries, &failure))
        {
            for (const SmbEntry &entry : entries)
                if (entry.folder)
                    answer.listing.names.push_back(entry.name);
            std::sort(answer.listing.names.begin(), answer.listing.names.end(),
                      [](const std::string &a, const std::string &b)
                      { return compare_names(a, b) < 0; });
            answer.listing.ok = true;
            answer.listing.choosable = ask->path != "/";
        }
        else
        {
            describe_smb_failure(ask->connection, failure, &answer.listing.headline,
                                 &answer.listing.detail);
        }
        std::lock_guard<std::mutex> hold(ask->shared->lock);
        ask->shared->answers.push_back(std::move(answer));
        delete ask;
        return nullptr;
    }
    if (ask->connection.type == ConnectionType::nfs)
    {
        // Above the exports, the folders leading to them; inside one, its
        // folders, any of which can be chosen.
        NfsSession session(ask->connection);
        std::vector<NfsEntry> entries;
        std::vector<std::string> exports;
        NfsFailure failure;
        if (session.list(ask->path, &entries, &failure) && session.list_exports(&exports, &failure))
        {
            for (const NfsEntry &entry : entries)
                if (entry.folder)
                    answer.listing.names.push_back(entry.name);
            std::sort(answer.listing.names.begin(), answer.listing.names.end(),
                      [](const std::string &a, const std::string &b)
                      { return compare_names(a, b) < 0; });
            answer.listing.ok = true;
            answer.listing.choosable = !nfs_location(exports, ask->path).exported.empty();
        }
        else
        {
            describe_nfs_failure(ask->connection, failure, &answer.listing.headline,
                                 &answer.listing.detail);
        }
        std::lock_guard<std::mutex> hold(ask->shared->lock);
        ask->shared->answers.push_back(std::move(answer));
        delete ask;
        return nullptr;
    }
    const HttpReply reply =
        http_request(ask->connection, "PROPFIND", ask->path,
                     "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
                     propfind_body());
    ProbeResult why;
    if (!reply.connected)
        why = interpret_socket_error(ask->connection, reply.error, true);
    else if (reply.status == 207 &&
             folders_from_listing(reply.body, ask->path, &answer.listing.names))
        answer.listing.ok = true;
    else if (reply.header.empty() && reply.body.empty())
        why = interpret_socket_error(ask->connection, reply.error != 0 ? reply.error : ECONNRESET,
                                     false);
    else
        why = interpret_response(ask->connection, reply.status, reply.header, reply.body);
    if (!answer.listing.ok)
    {
        answer.listing.headline = why.headline.empty() ? "Couldn't open this folder" : why.headline;
        answer.listing.detail = why.detail;
    }
    {
        std::lock_guard<std::mutex> hold(ask->shared->lock);
        ask->shared->answers.push_back(std::move(answer));
    }
    delete ask;
    return nullptr;
}

} // namespace

// ---- the picker ----

FolderPicker::FolderPicker(const ui::Fonts &fonts)
    : fonts_(fonts), shared_(std::make_shared<Shared>())
{
#if defined(__PROSPERO__)
    shared_->ui_cpu = sceKernelGetCurrentCpu();
#endif
}

FolderPicker::~FolderPicker() = default; // a listing still out finishes on its own

void FolderPicker::request(const std::string &path)
{
    Ask *ask = new Ask{shared_, connection_, path};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, &ask_main, ask);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete ask;
        Listing failed;
        failed.headline = "Couldn't start reading the NAS";
        listings_[path] = failed;
    }
}

void FolderPicker::open(const Connection &connection, const std::string &start,
                        app::Feedback &feedback)
{
    connection_ = connection;
    listings_.clear();
    remembered_.clear();
    {
        std::lock_guard<std::mutex> hold(shared_->lock);
        shared_->answers.clear(); // from an earlier opening
    }
    open_ = true;
    chosen_.clear();
    start_ = as_folder(start);
    path_ = old_path_ = start_;
    out_rows_.clear();
    focus_ = 0;
    pending_focus_.clear();
    scroll_.position.snap(0.0f);
    ring_.snap(0.0f);
    slide_ = arrive_ = crumb_ = 1.0f;
    age_ = 0.0f;
    request(start_);
    feedback.play(audio::Cue::modal_open);
}

const FolderPicker::Listing *FolderPicker::current() const
{
    const auto found = listings_.find(path_);
    return found == listings_.end() ? nullptr : &found->second;
}

void FolderPicker::take_results(app::Feedback *feedback)
{
    std::vector<Shared::Answer> answers;
    {
        std::lock_guard<std::mutex> hold(shared_->lock);
        answers.swap(shared_->answers);
    }
    for (Shared::Answer &answer : answers)
    {
        const bool shown = answer.path == path_;
        // The folder the box held can't be listed (it moved, or was typed
        // wrong): start at the top instead.
        if (shown && !answer.listing.ok && path_ == start_ && path_ != "/")
        {
            start_ = "/";
            path_ = old_path_ = "/";
            request("/");
            continue;
        }
        listings_[answer.path] = std::move(answer.listing);
        if (!shown)
            continue;
        const Listing &listing = listings_[path_];
        focus_ = 0;
        const auto memory = remembered_.find(path_);
        if (memory != remembered_.end())
            focus_ = memory->second;
        for (std::size_t i = 0; i < listing.names.size(); ++i)
            if (!pending_focus_.empty() && listing.names[i] == pending_focus_)
                focus_ = static_cast<int>(i);
        focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(listing.names.size()) - 1));
        ring_.snap(static_cast<float>(focus_));
        scroll_.position.snap(0.0f);
        reveal();
        scroll_.position.snap(scroll_.position.target);
        arrive_ = 0.0f;
        if (feedback != nullptr && !listing.ok)
            feedback->play(audio::Cue::error);
    }
}

void FolderPicker::show(const std::string &path, int direction, const std::string &focus_name)
{
    remembered_[path_] = focus_;
    const Listing *from = current();
    out_rows_ = from != nullptr && from->ok ? from->names : std::vector<std::string>{};
    out_focus_ = focus_;
    out_scroll_ = scroll_.offset();
    old_path_ = path_;
    path_ = path;
    direction_ = direction;
    slide_ = 0.0f;
    crumb_ = 0.0f;
    pending_focus_ = focus_name;
    focus_ = 0;
    if (listings_.count(path) > 0 && listings_[path].ok)
    {
        // Seen before in this opening: shown at once, where it was left.
        const Listing &listing = listings_[path];
        const auto memory = remembered_.find(path);
        focus_ = memory != remembered_.end() ? memory->second : 0;
        for (std::size_t i = 0; i < listing.names.size(); ++i)
            if (!focus_name.empty() && listing.names[i] == focus_name)
                focus_ = static_cast<int>(i);
        focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(listing.names.size()) - 1));
        ring_.snap(static_cast<float>(focus_));
        scroll_.position.snap(0.0f);
        reveal();
        scroll_.position.snap(scroll_.position.target);
        arrive_ = 0.0f;
    }
    else
    {
        listings_.erase(path);
        request(path);
    }
}

void FolderPicker::reveal()
{
    const Listing *listing = current();
    const float count = listing != nullptr ? static_cast<float>(listing->names.size()) : 0.0f;
    const float start = static_cast<float>(focus_) * kRowH;
    scroll_.reveal(start, start + kRowH, kListH, kRowH, count * kRowH);
}

void FolderPicker::refuse(app::Feedback &feedback, float dx, float dy)
{
    nudge_x_ = dx;
    nudge_y_ = dy;
    nudge_.trigger();
    feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
}

void FolderPicker::step(int delta, bool repeat, app::Feedback &feedback)
{
    const Listing *listing = current();
    if (listing == nullptr || !listing->ok || listing->names.empty())
        return;
    const int next = focus_ + delta;
    if (next < 0 || next >= static_cast<int>(listing->names.size()))
    {
        if (!repeat)
            refuse(feedback, 0.0f, static_cast<float>(delta));
        return;
    }
    focus_ = next;
    reveal();
    // Rows lower in the list tick lower, as in the File Browser.
    feedback.play(audio::Cue::focus,
                  1.04f - 0.08f * static_cast<float>(focus_) /
                              std::max(1.0f, static_cast<float>(listing->names.size())));
}

std::vector<std::string> FolderPicker::segments(const std::string &path) const
{
    std::vector<std::string> parts{connection_.host};
    std::size_t at = 1;
    while (at < path.size())
    {
        const std::size_t end = path.find('/', at);
        if (end == std::string::npos)
            break;
        parts.push_back(path.substr(at, end - at));
        at = end + 1;
    }
    return parts;
}

FolderPicker::Result FolderPicker::handle(const InputFrame &input, app::Feedback &feedback)
{
    if (!open_)
        return Result::none;
    take_results(&feedback);
    const Listing *listing = current();
    const bool ready = listing != nullptr;
    const int depth = static_cast<int>(segments(path_).size()) - 1;

    if (input.is_pressed(Action::back))
    {
        if (path_ == "/")
        {
            open_ = false;
            feedback.play(audio::Cue::modal_close);
            return Result::cancelled;
        }
        // Up: the folder you came from keeps the focus.
        show(parent_of(path_), -1, leaf_of(path_));
        feedback.play(audio::Cue::back, 1.06f - 0.04f * static_cast<float>(depth - 1));
        return Result::none;
    }
    if (input.is_pressed(Action::west))
    {
        // SMB's top lists shares, and the games folder is inside one; NFS's
        // folders above its exports can't be read either.
        if (!ready || !listing->ok || !listing->choosable)
        {
            refuse(feedback, -1.0f, 0.0f); // still reading, or it couldn't be read
            return Result::none;
        }
        chosen_ = path_.size() > 1 ? path_.substr(0, path_.size() - 1) : "/";
        open_ = false; // silently: the folder shows in its field
        return Result::chosen;
    }
    if (input.nav == Direction::up || input.nav == Direction::down)
        step(input.nav == Direction::down ? 1 : -1, input.nav_repeat, feedback);
    // Cross or right opens the focused folder; left goes up, like Circle.
    if (input.is_pressed(Action::confirm) ||
        (input.nav == Direction::right && !input.nav_repeat))
    {
        if (!ready || !listing->ok || listing->names.empty())
        {
            refuse(feedback, 1.0f, 0.0f);
            return Result::none;
        }
        const std::string &name = listing->names[static_cast<std::size_t>(focus_)];
        show(path_ + name + "/", 1, "");
        // Deeper sounds lower, like stairs going down.
        feedback.play(audio::Cue::open, 1.06f - 0.04f * static_cast<float>(depth + 1));
    }
    else if (input.nav == Direction::left && !input.nav_repeat)
    {
        if (path_ == "/")
        {
            refuse(feedback, -1.0f, 0.0f);
        }
        else
        {
            show(parent_of(path_), -1, leaf_of(path_));
            feedback.play(audio::Cue::back, 1.06f - 0.04f * static_cast<float>(depth - 1));
        }
    }
    return Result::none;
}

void FolderPicker::update(float dt)
{
    age_ += dt;
    shown_.target = open_ ? 1.0f : 0.0f;
    shown_.update(dt, open_ ? 14.0f : 20.0f);
    if (!open_)
        return;
    take_results(nullptr);
    ring_.target = static_cast<float>(focus_);
    ring_.update(dt, 22.0f);
    scroll_.update(dt);
    nudge_.update(dt, 9.0f);
    slide_ = std::min(1.0f, slide_ + dt / 0.34f);
    arrive_ = std::min(1.0f, arrive_ + dt / 0.40f);
    crumb_ = std::min(1.0f, crumb_ + dt / 0.42f);
}

void FolderPicker::draw_rows(gfx::DrawList &list, ui::Painter &paint,
                             const std::vector<std::string> &rows, int focus, float scroll,
                             float alpha, float dx, bool live, const Rect &area) const
{
    if (alpha <= 0.01f || rows.empty())
        return;
    const ui::Theme &theme = acrylic();
    list.push_opacity(alpha);
    list.push_transform(1.0f, 0.0f, 0.0f, dx, 0.0f);

    // The highlight: one object under the rows, gliding between them.
    const float at = live ? ring_.value : static_cast<float>(focus);
    Rect ring{area.x, area.y + at * kRowH - scroll, area.w, kRowH};
    if (live)
    {
        ring.x += ui::shake(nudge_.value, age_, 10.0f) * nudge_x_;
        ring.y += ui::shake(nudge_.value, age_, 6.0f) * nudge_y_;
    }
    ring = ring.inset(3.0f);
    list.shadow({ring.x, ring.y + 6.0f, ring.w, ring.h}, kRadius, 18, Color::rgb(0, 0.35f));
    list.glow(ring, kRadius, 12, theme.primary.with_alpha(0.14f + 0.08f * ui::breathe(age_)));
    list.bordered_rect(ring, kRadius, theme.primary.with_alpha(0.17f), 2.0f, theme.primary);

    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const float y = area.y + static_cast<float>(i) * kRowH - scroll;
        if (y > area.y + area.h || y + kRowH < area.y)
            continue;
        // On arrival the rows come in top to bottom, a beat apart.
        const float order = (y - area.y) / kRowH;
        const float appear =
            live ? tween::cubic_out((arrive_ * 0.40f - 0.025f * order) / 0.30f) : 1.0f;
        if (appear <= 0.01f)
            continue;
        const bool focused = static_cast<int>(i) == focus;
        const float cy = y + kRowH * 0.5f;
        list.push_opacity(appear);
        if (!focused)
            list.rounded_rect({area.x + 14.0f, y + kRowH - 0.5f, area.w - 28.0f, 1.0f}, 0.0f,
                              kWhite.with_alpha(0.045f));
        draw_folder(list, area.x + kIconX, cy, 30.0f);
        paint.body(ui::fit_label(paint, rows[i], kNameSize, area.w - kNameX - 60.0f),
                   area.x + kNameX, cy + kNameSize * 0.35f, kNameSize,
                   theme.text.with_alpha(focused ? 1.0f : 0.84f));
        // Every row is a folder that opens.
        draw_chevron(list, area.x + area.w - 30.0f, cy, 14.0f,
                     theme.text.with_alpha(focused ? 0.7f : 0.28f));
        list.pop_opacity();
    }
    list.pop_transform();
    list.pop_opacity();
}

void FolderPicker::draw(gfx::DrawList &list, std::uint32_t glass, float clock) const
{
    const float t = tween::clamp01(shown_.value);
    if (t <= 0.01f)
        return;
    (void)clock;
    const ui::Theme &theme = acrylic();
    ui::Painter paint(list, fonts_, theme, glass);

    // A veil over the Connection screen, then the panel rising into place.
    list.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f, Color::rgb(0, 0.5f * t));
    list.push_opacity(t);
    list.push_transform(0.94f + 0.06f * t, kPanel.cx(), kPanel.cy(), 0.0f, 24.0f * (1.0f - t));
    paint.panel(kPanel);

    const float x = kPanel.x + kPad;
    const float w = kPanel.w - kPad * 2.0f;
    paint.label("Choose the games folder", x, kPanel.y + 62.0f, 18, theme.primary);

    // ---- the breadcrumb: shared segments stay, the rest leave or arrive ----
    const std::vector<std::string> now = segments(path_);
    const std::vector<std::string> before = segments(old_path_);
    std::size_t shared = 0;
    while (shared < now.size() && shared < before.size() && now[shared] == before[shared])
        ++shared;
    const float crumb_y = kPanel.y + 112.0f;
    constexpr float kCrumb = 30.0f;
    const float way = direction_ < 0 ? -1.0f : 1.0f;
    // A long path keeps its last folders and shows the rest as "...".
    const auto widths = [&](const std::vector<std::string> &parts, std::size_t from)
    {
        float total = 0.0f;
        for (std::size_t i = from; i < parts.size(); ++i)
            total += paint.heading_width(parts[i], kCrumb) + (i == from ? 0.0f : 40.0f);
        return total;
    };
    std::size_t first = 0;
    while (first + 1 < now.size() && widths(now, first) > w - 60.0f)
        ++first;
    float cx = x;
    if (first > 0)
    {
        paint.heading("...", cx, crumb_y, kCrumb, theme.text.with_alpha(0.5f));
        cx += paint.heading_width("...", kCrumb);
    }
    const float arrive = tween::clamp01((crumb_ - 0.2f) / 0.8f);
    for (std::size_t i = first; i < now.size(); ++i)
    {
        const bool last = i + 1 == now.size();
        const bool moving = i >= shared;
        const float alpha = moving ? tween::smoothstep(arrive) : 1.0f;
        const float shift = moving ? way * 30.0f * (1.0f - tween::quint_out(arrive)) : 0.0f;
        if (i > first || first > 0)
        {
            draw_chevron(list, cx + 20.0f + shift, crumb_y - 11.0f, 14.0f,
                         theme.text.with_alpha(0.35f * alpha));
            cx += 40.0f;
        }
        const std::string shown = ui::fit_label(paint, now[i], kCrumb, 360.0f);
        paint.heading(shown, cx + shift, crumb_y, kCrumb,
                      theme.text.with_alpha((last ? 1.0f : 0.5f) * alpha));
        cx += paint.heading_width(shown, kCrumb);
    }

    // Under it, what this folder holds. SMB's top holds the NAS's shares,
    // and isn't a folder that can be chosen (nor are NFS's folders above its
    // exports).
    const Listing *listing = current();
    const bool shares = connection_.type == ConnectionType::smb && path_ == "/";
    std::string count;
    if (listing == nullptr)
        count = "Reading...";
    else if (!listing->ok)
        count = "Couldn't be read";
    else if (shares)
        count = std::to_string(listing->names.size()) +
                (listing->names.size() == 1 ? " share" : " shares");
    else
        count = std::to_string(listing->names.size()) +
                (listing->names.size() == 1 ? " folder" : " folders");
    paint.body(count, x, kPanel.y + 154.0f, 21, theme.text_muted);
    const float top = kPanel.y + kListTop - 26.0f;
    list.rounded_rect({kPanel.x + 24.0f, top, kPanel.w - 48.0f, 1.0f}, 0.0f,
                      kWhite.with_alpha(0.1f));

    // ---- the list: the old folder slides out, the new one in ----
    const Rect area{kPanel.x + 24.0f, kPanel.y + kListTop - 20.0f, kPanel.w - 48.0f, kListH};
    list.push_clip({area.x - 10.0f, area.y, area.w + 20.0f, area.h});
    if (slide_ < 1.0f)
    {
        const float leave = tween::clamp01(slide_ * 1.8f);
        draw_rows(list, paint, out_rows_, out_focus_, out_scroll_, 1.0f - tween::smoothstep(leave),
                  -way * 220.0f * tween::cubic_in(leave), false, area);
    }
    const float enter_dx = way * 240.0f * (1.0f - tween::quint_out(arrive_));
    if (listing != nullptr && listing->ok)
        draw_rows(list, paint, listing->names, focus_, scroll_.offset(),
                  tween::smoothstep(arrive_), enter_dx, true, area);
    list.pop_clip();

    // States, not blanks: reading, empty, or why it can't be read.
    const float mid_x = area.x + area.w * 0.5f;
    const float mid_y = area.y + area.h * 0.42f;
    if (listing == nullptr && slide_ > 0.5f)
    {
        // Three dots breathing in turn while the NAS answers.
        for (int i = 0; i < 3; ++i)
        {
            const float phase = 0.5f + 0.5f * std::sin(age_ * 5.0f - static_cast<float>(i) * 0.9f);
            list.circle(mid_x - 24.0f + static_cast<float>(i) * 24.0f, mid_y, 6.0f,
                        theme.primary.with_alpha(0.3f + 0.6f * phase));
        }
    }
    else if (listing != nullptr && listing->ok && listing->names.empty())
    {
        const float a = tween::smoothstep(arrive_);
        list.push_opacity(a);
        list.ring(mid_x, mid_y - 30.0f, 72.0f, 1.5f, kWhite.with_alpha(0.08f));
        draw_folder(list, mid_x, mid_y - 30.0f, 64.0f, 0.55f);
        paint.body(shares ? "No shares to show" : "No folders in here", mid_x, mid_y + 92.0f, 26,
                   theme.text, gfx::Align::center);
        paint.body(shares ? "This user can't see any shares. Check the user name on the NAS."
                          : "Choose this folder with Square, or go back up.",
                   mid_x, mid_y + 130.0f, 21, theme.text_muted, gfx::Align::center);
        list.pop_opacity();
    }
    else if (listing != nullptr && !listing->ok)
    {
        const float a = tween::smoothstep(arrive_);
        list.push_opacity(a);
        paint.body(ui::fit_label(paint, listing->headline, 26.0f, area.w - 80.0f), mid_x, mid_y,
                   26, theme.danger, gfx::Align::center);
        if (!listing->detail.empty())
            paint.body(ui::fit_label(paint, listing->detail, 21.0f, area.w - 80.0f), mid_x,
                       mid_y + 40.0f, 21, theme.text_muted, gfx::Align::center);
        list.pop_opacity();
    }

    // A thin thumb says where in a long folder you are.
    if (listing != nullptr && listing->ok)
    {
        const float content = static_cast<float>(listing->names.size()) * kRowH;
        if (content > kListH)
        {
            const Rect track{area.x + area.w + 4.0f, area.y + 4.0f, 3.0f, kListH - 8.0f};
            const float size = std::max(40.0f, track.h * kListH / content);
            const float at = tween::clamp01(scroll_.offset() / (content - kListH));
            list.rounded_rect(track, 1.5f, kWhite.with_alpha(0.07f));
            list.rounded_rect({track.x, track.y + (track.h - size) * at, 3.0f, size}, 1.5f,
                              theme.text.with_alpha(0.4f));
        }
    }

    // ---- the foot: what Square would choose, and the buttons ----
    const float foot = kPanel.y + kPanel.h - 92.0f;
    list.rounded_rect({kPanel.x + 24.0f, foot - 34.0f, kPanel.w - 48.0f, 1.0f}, 0.0f,
                      kWhite.with_alpha(0.1f));
    const std::string here = path_.size() > 1 ? path_.substr(0, path_.size() - 1) : "/";
    paint.body("Folder", x, foot, 19, theme.text_muted);
    paint.body(ui::fit_label(paint, here, 23.0f, w - 90.0f), x + 90.0f, foot, 23, theme.text);
    ui::HintLayout layout;
    layout.size = 34.0f;
    layout.text_size = 22.0f;
    layout.cy = kPanel.y + kPanel.h - 40.0f;
    layout.item_gap = 34.0f;
    const ui::Hint hints[] = {{ui::Button::cross, "Open"},
                              {ui::Button::square, "Use this folder"},
                              {ui::Button::circle, path_ == "/" ? "Close" : "Up"}};
    // Without Square where a folder can't be chosen (SMB's top, where a share
    // has to be opened first, and NFS's folders above its exports).
    const ui::Hint top_hints[] = {hints[0], hints[2]};
    if (shares || (listing != nullptr && listing->ok && !listing->choosable))
        ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), top_hints, 2,
                       kPanel.x + kPanel.w - kPad, true, layout);
    else
        ui::draw_hints(list, fonts_, ui::GlyphStyle::dark(), hints, 3, kPanel.x + kPanel.w - kPad,
                       true, layout);

    list.pop_transform();
    list.pop_opacity();
}

} // namespace hui::concepts
