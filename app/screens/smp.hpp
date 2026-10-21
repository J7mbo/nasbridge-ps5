// NASBridge - Talking to ShadowMountPlus.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShadowMountPlus (the payload that registers games on the home screen) has
// an HTTP/JSON API on 127.0.0.1:10101: every call is a POST of one JSON
// object (its docs/api.md). NASBridge uses it to know which games are on the
// PS5, to have a copied game found at once (a rescan), and to delete a game
// (its files, then its home-screen entry). Every call blocks: worker threads
// only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hui::concepts
{

// A game ShadowMountPlus knows about.
struct SmpGame
{
    std::string title_id;
    std::string title_name;
    std::string path;        // the folder, the image, or (installed PKGs) app.pkg
    std::string source_type; // "folder", "image" or "pkg"
    bool installed = false;  // registered on the home screen
    bool mounted = false;    // running (deleting is refused then)
    bool can_uninstall = false;
    bool can_manage_source = false; // its files can be deleted through the API
};

// One storage job (ShadowMountPlus runs one at a time: copy, move, delete).
struct SmpJob
{
    int id = 0;
    std::string operation; // "delete", ...
    std::string state;     // preparing, measuring, transferring, deleting, finalizing,
                           // completed, failed, cancelled
    bool active = false;
    std::uint64_t processed_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::string error;  // result_error, when it failed
    std::string reason; // result_error_reason ("game_active", ...)
};

// Where its API listens: 127.0.0.1:10101. Tests move it, so they can run side by side.
int smp_port();
void set_smp_port(int port);

// Every game it knows (include_size false: no folder walks).
bool smp_games(std::vector<SmpGame> *games, std::string *why);
// Asks for an immediate full rescan. reset_attempts clears its retry counters,
// so a title it gave up on is tried again. *deferred, when given, is why the
// scan waits ("" when it runs now). "game_active": a ShadowMountPlus older
// than 1.7beta6 counts NASBridge itself as a running game, and scans once it
// is closed. (From 1.7beta6 it doesn't, for a homebrew app: NASBridge's title
// ID starts with LAPY.)
bool smp_scan(bool reset_attempts, std::string *why, std::string *deferred = nullptr);
// Starts deleting a game's files. *job_id is the job to follow; *missing is
// true when the files were already gone (no job then).
// *reason, when given, is ShadowMountPlus's error_reason on a refusal
// ("game_active", "runtime_prepared", "title_unmount_busy", ...).
bool smp_delete_source(const std::string &title_id, int *job_id, bool *missing, std::string *why,
                       std::string *reason = nullptr);
bool smp_job(int job_id, SmpJob *job, std::string *why);
// Removes a game from the home screen. A game whose files are attached (but
// that isn't running) is released by ShadowMountPlus first.
bool smp_uninstall(const std::string &title_id, std::string *why, std::string *reason = nullptr);
// A refusal's error_reason, said plainly ("" when there's nothing to add).
std::string smp_reason_text(const std::string &reason);
// What to do about that refusal, as a sentence ("Try again in a moment.").
std::string smp_reason_hint(const std::string &reason);

// For tests.
bool smp_games_from_json(const std::string &json, std::vector<SmpGame> *games);
bool smp_job_from_json(const std::string &json, SmpJob *job);

} // namespace hui::concepts
