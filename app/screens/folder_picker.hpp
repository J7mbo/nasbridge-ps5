// NASBridge - Choosing the games folder on the NAS.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A frosted panel over the Connection screen, in the File Browser design's
// language (the kit's files.cpp): a breadcrumb that grows a segment per
// level, solid folder icons, one gliding highlight, folders that slide in the
// direction of travel, and every folder remembering where its focus was. It
// only lists folders and only chooses one: Cross opens the focused folder,
// Square chooses the folder being shown, Circle goes up (and closes at the
// top). Listings come from the NAS on a worker thread (WebDAV: PROPFIND,
// Depth 1; SMB: the NAS's shares at the top, then the folders in them; a
// USB drive: the drives plugged in, then their folders), with the connection
// as it is being edited.

#pragma once

#include "concepts/connection.hpp"

#include "app/concept.hpp"
#include "core/tween.hpp"
#include "ui/components.hpp"
#include "ui/motion.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hui::concepts
{

// The File Browser's folder icon (also on the Connection screen's button).
void draw_folder_icon(gfx::DrawList &list, float cx, float cy, float size, float alpha = 1.0f);

// One folder's sub-folders, sorted (for tests: the parsing part).
bool folders_from_listing(const std::string &body, const std::string &path,
                          std::vector<std::string> *names);

class FolderPicker
{
  public:
    explicit FolderPicker(const ui::Fonts &fonts);
    ~FolderPicker();
    FolderPicker(const FolderPicker &) = delete;
    FolderPicker &operator=(const FolderPicker &) = delete;

    // Opens on `start` ("/games"; "" or "/" for the top), listed through
    // `connection`. A start that can't be listed falls back to the top.
    void open(const Connection &connection, const std::string &start, app::Feedback &feedback);
    bool is_open() const
    {
        return open_;
    }
    // Still drawn: open, or fading out.
    bool visible() const
    {
        return open_ || shown_.value > 0.01f;
    }

    enum class Result
    {
        none,
        chosen,    // chosen() holds the folder ("/games/PS5", or "/")
        cancelled, // closed without a choice
    };
    // While open it takes every input.
    Result handle(const InputFrame &input, app::Feedback &feedback);
    const std::string &chosen() const
    {
        return chosen_;
    }
    // Open, and the folder shown hasn't been listed yet.
    bool waiting() const
    {
        return open_ && current() == nullptr;
    }

    void update(float dt);
    void draw(gfx::DrawList &list, std::uint32_t glass, float clock) const;

    struct Shared; // the worker's mailbox (folder_picker.cpp)

  private:
    struct Listing
    {
        bool ok = false;
        std::vector<std::string> names;
        std::string headline; // why it failed
        std::string detail;
    };

    void request(const std::string &path);
    void take_results(app::Feedback *feedback);
    void show(const std::string &path, int direction, const std::string &focus_name);
    void step(int delta, bool repeat, app::Feedback &feedback);
    void refuse(app::Feedback &feedback, float dx, float dy);
    void reveal();
    std::vector<std::string> segments(const std::string &path) const;
    const Listing *current() const;
    void draw_rows(gfx::DrawList &list, ui::Painter &paint, const std::vector<std::string> &rows,
                   int focus, float scroll, float alpha, float dx, bool live,
                   const gfx::Rect &area, bool drives) const;
    // Whether the top level holds places to open, not folders to choose
    // (SMB's shares, the drives).
    bool top_only() const;

    const ui::Fonts &fonts_;
    Connection connection_;
    std::shared_ptr<Shared> shared_;
    bool open_ = false;
    std::string path_ = "/";    // shown now: "/" or "/games/PS5/"
    std::string old_path_ = "/"; // before the last move (for the breadcrumb)
    std::map<std::string, Listing> listings_;  // by path, for this opening
    std::map<std::string, int> remembered_;    // focus by path
    std::string pending_focus_;                 // a name to land on when the list arrives
    std::string start_;                         // where open() wanted to begin
    int focus_ = 0;
    ui::Scroller scroll_;
    tween::Spring ring_;  // the highlight's row, gliding
    tween::Spring shown_; // the panel, 0..1
    // The folder being left slides out while the new one slides in.
    std::vector<std::string> out_rows_;
    int out_focus_ = 0;
    float out_scroll_ = 0.0f;
    int direction_ = 1;    // +1 deeper, -1 up
    float slide_ = 1.0f;   // 0..1 since the last move
    float arrive_ = 1.0f;  // 0..1 since the shown folder's rows arrived
    float crumb_ = 1.0f;   // 0..1 for the breadcrumb
    ui::Pulse nudge_;
    float nudge_x_ = 0.0f;
    float nudge_y_ = 0.0f;
    std::string chosen_;
    float age_ = 0.0f;
};

} // namespace hui::concepts
