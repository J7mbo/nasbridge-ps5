// NASBridge - The copy queue, as a panel that slides in over the library.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// In the Now Playing design's language (the kit's player.cpp): real frosted
// glass sliding in from the right, a "QUEUE" label, one highlight springing
// between the rows, and the kit's Arc spinner on the game being copied. At
// the bottom, two of the kit's stat tiles: the live speed, with a graph of
// the last half minute, and how many connections are bringing data in.
// Moving a game is the Satchel design's lift and carry (inventory.cpp):
// Cross lifts it, up and down carry it, Cross (or Circle) sets it down.
//
//   up / down  focus, or carry the lifted game
//   Cross      lift / set down
//   Square     pause / resume the queue
//   Triangle   remove the focused game (asks first when it has copied files)
//   Circle     set down, or close

#pragma once

#include "concepts/connection.hpp"
#include "concepts/storage.hpp"
#include "concepts/transfer.hpp"

#include "app/concept.hpp"
#include "core/tween.hpp"
#include "ui/components.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/stat.hpp"
#include "ui/motion.hpp"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hui::concepts
{

class QueuePanel
{
  public:
    // A game's cover, as the library has it.
    struct Cover
    {
        std::uint32_t texture = 0; // 0: none (the colours stand in)
        gfx::Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
        gfx::Color top = gfx::Color::rgb(0x2a3140);
        gfx::Color bottom = gfx::Color::rgb(0x161a22);
    };
    using CoverOf = std::function<Cover(const std::string &entry)>;

    QueuePanel(const ui::Fonts &fonts, Transfers &transfers);

    void open(app::Feedback &feedback);
    bool is_open() const
    {
        return open_;
    }
    bool visible() const
    {
        return open_ || shown_.value > 0.01f;
    }

    // While open it takes every input. `connection` is what Resume copies from.
    void handle(const InputFrame &input, app::Feedback &feedback, const Connection &connection);
    void update(float dt, const QueueStatus &queue);
    void draw(gfx::DrawList &list, std::uint32_t glass, float clock, const QueueStatus &queue,
              const CoverOf &cover_of) const;
    // A folder game just removed with copied files to delete (the panel has
    // closed for its page to show the deleting), once; "" otherwise.
    std::string take_removed()
    {
        return std::exchange(removed_, std::string());
    }

  private:
    void refuse(app::Feedback &feedback, const std::string &why, float dy = 0.0f);
    void reveal();

    const ui::Fonts &fonts_;
    Transfers &transfers_;
    QueueStatus queue_; // as of the last update
    bool open_ = false;
    int focus_ = 0;
    bool lifted_ = false;
    tween::Spring shown_;
    tween::Spring ring_; // the highlight's row
    tween::Spring lift_; // 0..1: how lifted the focused row is
    ui::Scroller scroll_;
    std::map<std::string, tween::Spring> row_y_; // each game's row, gliding when it moves
    ui::Pulse nudge_;
    float nudge_y_ = 0.0f;
    std::string notice_; // why the last press was refused
    float notice_age_ = 100.0f;
    ui::Spinner spinner_;     // on the game being copied
    ui::StatTile speed_tile_; // MB/s, with its graph
    ui::StatTile links_tile_; // connections delivering data
    std::vector<float> speeds_; // MB/s, oldest first, one per sample
    std::vector<float> links_;
    float sample_clock_ = 0.0f;
    ui::Dialog confirm_; // "Remove ... from the queue?"
    std::string confirm_entry_;
    bool confirm_image_ = false;
    std::string removed_;
    float age_ = 0.0f;
};

} // namespace hui::concepts
