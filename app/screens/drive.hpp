// NASBridge - A drive plugged into the PS5 as the place games are read from.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "USB drive" is the third connection type, beside WebDAV and SMB: the games
// folder is a full path on a drive the PS5 has mounted ("/mnt/usb0/Games").
// ShadowMountPlus gives homebrew apps (a LAPY title ID, as NASBridge has)
// their own view of /mnt/usb0..7 and /mnt/ext0..1, following hotplug, so
// NASBridge reads them directly. Listings come back as DavEntry, as from a
// NAS, with absolute paths.
//
// A drive game ShadowMountPlus already runs from the drive (one in its scan
// paths: the drive's top, homebrew or etaHEN/games) keeps winning over a copy
// on the PS5 while the drive is plugged in. So once that game is copied, its
// folder (or image) on the drive is renamed to start with a dot, which
// ShadowMountPlus skips, and the PS5 copy takes over. Each rename is kept in
// a list in NASBridge's own folder; when the PS5 copy is deleted, the drive
// copy is renamed back (now, or when its drive is next plugged in). The list
// is only a hint: the drive is checked every time, matching the hidden copy
// by its title ID.

#pragma once

#include "concepts/http.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hui::concepts
{

// ---- the drives ----

// Every place a drive can be mounted: /mnt/usb0 .. /mnt/usb7, /mnt/ext0, /mnt/ext1.
const std::vector<std::string> &drive_roots();
// Those mounted now ("/mnt/usb0", ...).
std::vector<std::string> drive_roots_present();
// "/mnt/usb0" for any path on that drive ("/mnt/usb0/Games/X"); "" when the
// path isn't on a drive.
std::string drive_root_of(const std::string &path);
// Whether the drive `path` is on is plugged in now.
bool drive_root_present(const std::string &path);
// "USB drive 1" for /mnt/usb0, "Extended storage" for /mnt/ext0, ...
std::string drive_name(const std::string &root);

// Tests: drives are looked for under `prefix` ("/tmp/x" makes /mnt/usb0
// /tmp/x/mnt/usb0); paths given and returned keep their /mnt form.
void set_drive_prefix(const std::string &prefix);
// The path to open for `path` ("/mnt/usb0/x"): itself, except in tests.
std::string drive_real_path(const std::string &path);

// ---- reading it ----

// A folder's entries ("/mnt/usb0/Games/"), without hidden and system ones
// (".x", "System Volume Information", "$RECYCLE.BIN"). False with errno in
// *error.
bool drive_list(const std::string &folder, std::vector<DavEntry> *entries, int *error);
// A whole small file, at most `max` bytes (*too_big when it's longer).
bool drive_read_file(const std::string &path, std::size_t max, std::string *out, bool *too_big,
                     int *error);
// Exactly `length` bytes at `offset`.
bool drive_read_range(const std::string &path, std::uint64_t offset, std::size_t length,
                      std::string *out, int *error);
// A failure in a few words: "not found", "the drive isn't connected".
std::string drive_error_brief(const std::string &path, int error);

// ---- hiding a drive copy from ShadowMountPlus ----

struct HiddenCopy
{
    std::string title_id;
    std::string path;   // where it was ("/mnt/usb0/homebrew/Astro Bot")
    std::string hidden; // where it is now ("/mnt/usb0/homebrew/.Astro Bot")
    bool image = false; // an .exfat file, not a folder
    bool unhide = false; // its PS5 copy was deleted: rename it back when the drive is here
};

// Renames `path` (a game folder, or an image) to the same name with a dot in
// front, and remembers it. False with *why (nothing changed then).
bool hide_drive_copy(const std::string &title_id, const std::string &path, bool image,
                     std::string *why);
// The hidden copy remembered for a title; false when there's none.
bool hidden_copy(const std::string &title_id, HiddenCopy *copy);
std::vector<HiddenCopy> hidden_copies();

enum class Unhide
{
    none,     // nothing was hidden for this title (or it's gone from the drive)
    done,     // renamed back: ShadowMountPlus finds it at its next scan
    later,    // its drive isn't plugged in: done when it is (finish_unhides)
    failed,   // the drive is there but the rename failed (*why)
};
// The PS5 copy of `title_id` is gone: renames its drive copy back. The copy
// is looked for on every drive that's plugged in (it may be in another port
// now), by its title ID, in the folder it was hidden in.
Unhide unhide_drive_copy(const std::string &title_id, std::string *why);
// Tries every rename back still waiting for its drive; how many were done.
int finish_unhides();
// Forgets entries whose drive is plugged in but whose hidden copy is gone
// (deleted, or renamed back by hand).
void prune_hidden();

// The warning before deleting a game whose drive copy is hidden while that
// drive isn't plugged in: true once "Don't warn again" was chosen.
bool hidden_warning_off();
void set_hidden_warning_off(bool off);

// Whether a drive for `copy` is plugged in now and holds it (hidden), by its
// title ID. *where gets the hidden copy's path on whichever drive has it.
bool find_hidden_copy(const HiddenCopy &copy, std::string *where);

} // namespace hui::concepts
