// NASBridge - "Test connection": one listing from the NAS (WebDAV, SMB or FTP), off the UI thread.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asks the NAS for the games folder's contents the way the library will
// (WebDAV: PROPFIND, Depth: 1; SMB: signing in and listing the folder, or the
// shares when no folder is set yet; FTP: signing in and listing the folder),
// and turns the answer into words:
// connected and what is there, or what went wrong and what to check.
//
// The request runs on a thread of its own with a 1 MiB stack (console
// threads are never time-sliced, and the default stack is small); the screen
// polls result() each frame. WebDAV: plain HTTP and Basic sign-in only for now.

#pragma once

#include "concepts/connection.hpp"

#include "concepts/http.hpp"

#include <atomic>
#include <functional>
#include <pthread.h>
#include <string>
#include <vector>

namespace hui::concepts
{

struct ProbeResult
{
    enum class State : int
    {
        idle,    // never run, or the settings changed since
        running, // waiting for the NAS
        ok,      // the folder was listed
        failed,  // headline and detail say why
    };
    State state = State::idle;
    std::string headline; // "Connected", "Wrong user name or password"
    std::string detail;   // what was found, or what to check
};

// The pure part, for tests: what an HTTP response means for the player.
// status is the HTTP status code (0 when the reply wasn't HTTP); body is the
// decoded body; header is the raw header block (for WWW-Authenticate and
// Location).
ProbeResult interpret_response(const Connection &connection, int status,
                               const std::string &header, const std::string &body);
// What a failed connect or read means (an errno value).
ProbeResult interpret_socket_error(const Connection &connection, int error, bool connecting);

// How many games the games folder holds, found the way the library finds
// them: each folder is a game folder when it has an sce_sys (up to four
// folders down), else an image game when it has an .exfat; .exfat files in
// the games folder itself are image games too.
struct GameCount
{
    int folder_games = 0;
    int image_games = 0;
    int other_folders = 0; // no game inside (patches, backports, downloads...)
    int unreadable = 0;    // couldn't be listed (the library shows them with a note)
};
// `top` is the games folder's listing. stop() ends it early (false then);
// progress(done, total) follows the folders.
bool count_games(const Connection &connection, const std::vector<DavEntry> &top,
                 const std::function<bool()> &stop,
                 const std::function<void(int done, int total)> &progress, GameCount *count);
// The test's line for a count ("Found 27 games: 12 folder games and 15 .exfat games").
std::string describe_count(const Connection &connection, const GameCount &count);

class NasProbe
{
  public:
    NasProbe() = default;
    ~NasProbe();
    NasProbe(const NasProbe &) = delete;
    NasProbe &operator=(const NasProbe &) = delete;

    // Starts a test of `connection`. While an older one is still out, this
    // one waits for it (see poll()). count_games: also count the games (a
    // listing per folder in the games folder, a second or two); without it
    // the test only checks the folder can be read.
    void start(const Connection &connection, bool count_games = false);
    // While counting: folders checked so far, of how many (0 of 0 before).
    void progress(int *done, int *total) const
    {
        *done = counted_.load();
        *total = to_count_.load();
    }
    // Starts a test that start() had to queue behind an older one. Call once a frame.
    void poll();
    // Forgets the last result (the settings changed); a running test's answer
    // is dropped when it arrives.
    void reset();
    bool running() const
    {
        return state_.load() == static_cast<int>(ProbeResult::State::running);
    }
    // A copy of the latest result; safe to call every frame.
    ProbeResult result() const;

  private:
    static void *thread_main(void *self);
    void run();

    Connection connection_;
    bool count_ = false;
    std::atomic<int> counted_{0};
    std::atomic<int> to_count_{0};
    std::atomic<bool> stop_{false}; // ends a count early (settings changed, closing)
    bool pending_count_ = false;
    std::atomic<int> state_{static_cast<int>(ProbeResult::State::idle)};
    std::atomic<int> generation_{0}; // bumped by reset(): stale answers are dropped
    int running_generation_ = 0;
    ProbeResult finished_;           // written by the worker before state_ is published
    bool have_thread_ = false;
    pthread_t thread_{};
    Connection pending_;
    bool has_pending_ = false;
};

} // namespace hui::concepts
