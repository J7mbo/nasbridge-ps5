// NASBridge - The copy queue, and deleting games from the PS5.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Games are added to a queue and copied one after another, on one worker
// thread, front of the queue first. Before each one starts, the free space on
// its drive is asked again: if what it still needs doesn't fit, the queue
// stops there ("blocked") until space is made or the game is moved or
// removed. The queue is saved, so it comes back after NASBridge closes (paused).
//
// Copying one game. The game's files are listed on the NAS (a folder game: the folder
// holding sce_sys, without .zip/.rar/.7z downloads; an image game: the .exfat
// file), then streamed one file at a time, each in one long GET, into
//   <destination>/.nasbridge-incoming/<name>
// ShadowMountPlus skips names starting with a dot, so a half-copied game is
// never registered. When every file is in, that folder is renamed to
// <destination>/<name> (instant: same drive) and ShadowMountPlus is asked to
// rescan at once.
//
// Pause stops the queue after the piece being written. Resume carries on from what is
// on the drive: each file's size says how far it got (the last 4 MiB are
// fetched again, in case the end of the file never reached the disk). The
// plan (the file list, where it goes) is saved in /download0/nasbridge, so
// a copy interrupted by closing NASBridge, a crash or a power cut comes back
// paused at the next launch. A dropped connection (the PS5 suspending the
// app, the NAS restarting) is retried by itself before giving up.
//
// Removing a game from the queue deletes whatever it had copied (NASBridge
// knows each file from the plan; it can't list folders in its sandbox). The
// game being copied can only be removed once the queue is paused.
//
// Deleting a game on the PS5 is separate (it can run while the queue copies):
// ShadowMountPlus deletes its files, then removes it from the home screen.

#pragma once

#include "concepts/connection.hpp"
#include "concepts/nas_library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hui::concepts
{

struct CopyFile
{
    std::string path; // relative to the game ("sce_sys/param.json"); "" for an image
    std::uint64_t size = 0;
};

// What a copy does; saved, so it can be resumed after NASBridge closes.
struct CopyPlan
{
    std::string entry;    // the game's key on the NAS (NasGame::entry)
    std::string title;
    std::string title_id;
    bool image = false;
    std::string source;      // on the NAS, decoded: the folder ("/games/Game/") or the file
    std::string destination; // "/data/homebrew", "/mnt/usb0/homebrew", ...
    std::string name;        // the folder or file name it gets there
    std::string nas;         // which NAS (host, port, folder): resumed only from the same one
    bool listed = false;     // the file list is complete
    bool moving = false;     // every file is in; the move into place has begun
    std::vector<std::string> folders; // relative, parents before children
    std::vector<CopyFile> files;
    std::uint64_t total = 0;
};

enum class TransferKind
{
    none,
    remove, // deleting a game that is on the PS5
};

// The phases of the game being copied, and of a delete from the PS5.
enum class TransferState
{
    idle,      // nothing going on
    preparing, // listing the game's files on the NAS
    copying,
    finishing, // moving the game into place and asking for a rescan
    failed,    // stopped by an error: `message` says why
    deleting,  // ShadowMountPlus is deleting a game on the PS5
    deleted,   // that game is gone
};

// Deleting a game that is on the PS5.
struct TransferStatus
{
    TransferKind kind = TransferKind::none;
    TransferState state = TransferState::idle;
    std::string entry; // which game (NasGame::entry); title_id too
    std::string title;
    std::string title_id;
    std::uint64_t done_bytes = 0; // ShadowMountPlus's delete job, so far
    std::uint64_t total_bytes = 0;
    std::string message;
    // A folder game off the home screen with files still to delete (while
    // the helper deletes them, and after it stops short): not on the PS5 any
    // more, but not ready to be copied again either.
    bool leftover = false;

    bool busy() const
    {
        return state == TransferState::deleting;
    }
};

enum class QueueState
{
    idle,    // nothing queued
    running, // copying the front game
    paused,  // stopped by Pause (or NASBridge closing); Resume carries on
    blocked, // the front game doesn't fit its drive: `message` says by how much
    failed,  // the front game hit an error: `message` says which; Resume retries
};

// One game in the queue, as the screen sees it.
struct QueueEntry
{
    std::string entry; // NasGame::entry: the game's key
    std::string title;
    std::string title_id;
    std::string destination;
    std::uint64_t size = 0; // bytes it copies (estimated until its files are listed)
    std::uint64_t done = 0; // bytes already on the drive
    bool started = false;   // has files on the drive (removing it deletes them)
};

struct QueueStatus
{
    QueueState state = QueueState::idle;
    std::string message;
    std::vector<QueueEntry> entries; // front first; [0] is the one copying
    // The front game while the queue runs:
    TransferState phase = TransferState::idle; // preparing, copying or finishing
    std::string detail;                        // "Connection lost, trying again", ...
    double rate = 0.0;                         // bytes per second, smoothed
    int connections = 0;                       // NAS connections delivering data now
    std::string conn_report;                   // a per-connection diagnostic line
    int files_done = 0;
    int files_total = 0;
    // Games whose files are being deleted after leaving the queue.
    std::vector<std::string> discarding;
    // The last game copied (it stays "Copied" until ShadowMountPlus lists it).
    std::string finished_entry;
    std::string finished_destination;
    // Copied games ShadowMountPlus adds only once NASBridge is closed: it
    // counts NASBridge as a running game and holds its scans until then.
    struct Waiting
    {
        std::string entry;
        std::string destination;
    };
    std::vector<Waiting> waiting_for_close;
    const Waiting *waiting(const std::string &entry) const
    {
        for (const Waiting &item : waiting_for_close)
            if (item.entry == entry)
                return &item;
        return nullptr;
    }

    int find(const std::string &entry) const
    {
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].entry == entry)
                return static_cast<int>(i);
        return -1;
    }
    bool running() const
    {
        return state == QueueState::running;
    }
    // Bytes the queue will still write to `destination`.
    std::uint64_t queued_for(const std::string &destination) const
    {
        std::uint64_t bytes = 0;
        for (const QueueEntry &item : entries)
            if (item.destination == destination)
                bytes += item.size > item.done ? item.size - item.done : 0;
        return bytes;
    }
};

class Transfers
{
  public:
    Transfers();
    // A running copy stops where it is and stays resumable; nothing waits.
    ~Transfers();
    Transfers(const Transfers &) = delete;
    Transfers &operator=(const Transfers &) = delete;

    // Picks up the queue saved by an earlier run (paused). Call once.
    void load();

    // Each returns false (with *why) when it can't be done now.
    // Adds a game at the end; starts copying when the queue was empty.
    bool enqueue(const Connection &connection, const NasGame &game, const std::string &title,
                 const std::string &destination, std::string *why);
    bool pause();
    bool resume(const Connection &connection, std::string *why);
    // Moves the game at `index` one place (delta -1 up, +1 down). The game
    // being copied stays put while the queue runs.
    bool move(int index, int delta, std::string *why);
    // Takes the game at `index` out of the queue and deletes what it had
    // copied. The game being copied must be paused first.
    bool remove(int index, std::string *why);

    // Deletes a game that is on the PS5: its files, then its home-screen entry.
    bool remove_installed(const std::string &title_id, const std::string &entry,
                          const std::string &title, std::string *why);

    QueueStatus queue() const;
    TransferStatus removal() const;

    // Where the queue asks how much room a drive has (before each game).
    // Returns false when it can't tell (the game starts anyway). The default
    // asks ShadowMountPlus.
    using SpaceCheck = std::function<bool(const std::string &destination, std::uint64_t *free)>;
    void set_space_check(SpaceCheck check);

    struct Shared; // what the screen and the worker threads share (transfer.cpp)

  private:
    std::shared_ptr<Shared> shared_;
};

// ---- for tests ----
std::string plan_to_text(const CopyPlan &plan);
bool plan_from_text(const std::string &text, CopyPlan *plan);
// Where a NAS connection is, as saved with a plan.
std::string nas_identity(const Connection &connection);
// The NAS folder a folder game is copied from (the one holding sce_sys), and
// the name it gets on the PS5.
std::string copy_source(const Connection &connection, const NasGame &game);
std::string copy_name(const NasGame &game);
// The queue as saved, and back.
struct SavedItem
{
    CopyPlan plan;
    std::uint64_t estimate = 0; // bytes, before the plan is listed
    std::uint64_t done = 0;
    bool started = false;
};
std::string queue_to_text(const std::vector<SavedItem> &items);
bool queue_from_text(const std::string &text, std::vector<SavedItem> *items);
// Moves the saved queue somewhere else.
void set_transfer_root(const std::string &root);
// How many connections a copy uses (clamped 1..4). Default 1, best for a single
// spinning HDD on the NAS. Tests raise it to exercise the parallel path.
void set_copy_streams(int streams);
// How many files the delete helper deletes at once for a folder game (1..32).
void set_delete_threads(int threads);
// How long a deep listing may stay silent before listing folder by folder
// (seconds; default 8). Also forgets which NASes didn't list deeply.
void set_deep_listing_wait(int seconds);

} // namespace hui::concepts
