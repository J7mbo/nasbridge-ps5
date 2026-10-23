// NASBridge - How much room the PS5's own storage has.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The number the PS5's Settings shows as free console storage. NASBridge runs
// sandboxed: it can't see /user, and statfs on its own folders is refused. So
// it asks ShadowMountPlus, which runs outside the sandbox and answers on
// 127.0.0.1:10101 (POST /api/v1/storage: every mount with its total and
// available bytes; the same API ps5-exporter reads). The ways NASBridge
// could ask itself stay behind it as a fallback. Asked on a background thread
// every few seconds; the screen reads the latest answer. The PC preview
// reports a fixed demo value.

#pragma once

#include "concepts/smp.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hui::concepts
{

// A folder ShadowMountPlus scans for games, where a copy can go
// (/data/homebrew, /mnt/ext0/homebrew, /mnt/usb0/homebrew, ...).
struct Destination
{
    std::string path;
    std::string mount_point;
    std::uint64_t free = 0;  // bytes available for new files
    std::uint64_t total = 0; // bytes
    bool read_only = false;
    // Whether NASBridge itself can create, write, reopen, rename and remove a
    // file there (-1: not checked yet). NASBridge runs sandboxed, so what
    // ShadowMountPlus can write isn't proof.
    int writable = -1;
    std::string why; // the step that failed ("create e1"), when not writable
};

// "Internal storage", "Extended storage", "USB drive 1", or the path.
std::string destination_name(const std::string &path);

// Where ShadowMountPlus finds games on the console's own storage.
constexpr const char *kInternalGames = "/data/homebrew";

// The drive a path is on ("/mnt/usb0" for "/mnt/usb0/homebrew/Game"; the
// drives are /mnt/usb0..7 and /mnt/ext0..1), or "" when it's not on one.
std::string drive_of(const std::string &path);

// A drive's mount, as ShadowMountPlus lists it.
struct DriveSpace
{
    std::string mount_point; // "/mnt/usb0"
    std::uint64_t free = 0;  // bytes
    std::uint64_t total = 0; // bytes
    bool read_only = false;
};

struct StorageSpace
{
    bool asked = false;      // false until the first answer arrives
    bool known = false;
    std::uint64_t free = 0;  // bytes
    std::uint64_t total = 0; // bytes (0 when unknown)
    std::string how;         // which way answered, or (unknown) why each way failed
    // Where games can be copied: Internal storage first (always there), then
    // one folder per plugged-in drive (<drive>/homebrew, which ShadowMountPlus
    // scans). See merge_destinations().
    std::vector<Destination> destinations;
    // The drives plugged in now ("/mnt/usb0", ...), and whether that list is
    // an answer (false until ShadowMountPlus or NASBridge itself could tell).
    bool drives_known = false;
    std::vector<std::string> drives;
    std::vector<DriveSpace> drive_space; // their mounts, when ShadowMountPlus listed them
    // The games ShadowMountPlus knows (on the PS5 or a drive); games_known is
    // false until it has answered once.
    bool games_known = false;
    std::vector<SmpGame> games;
};

// Checks that NASBridge can write in `folder` the way a copy does (a scratch
// file is made and removed again). Sets writable and why. Blocks: worker
// threads only.
void check_destination(Destination *destination);

// Asks NASBridge's own process (no ShadowMountPlus): system calls only.
StorageSpace internal_storage();

// Asks ShadowMountPlus (blocks up to ~15 s when it doesn't answer: call from
// a worker thread).
StorageSpace shadowmount_storage();

// For tests: the free space of the console's storage in a /api/v1/storage
// reply, or why there is none in space->how. Its drives are the mounts on
// /mnt/usbN and /mnt/extN.
StorageSpace storage_from_json(const std::string &json);

// Whether the drive `path` is on is plugged in. Paths that aren't on a drive
// (Internal storage), and any path while the drives aren't known, count as
// connected.
bool drive_connected(const StorageSpace &space, const std::string &path);

// Turns what ShadowMountPlus listed (space->destinations) and the drives
// plugged in (space->drives) into the choices for a copy: Internal storage
// first, even when ShadowMountPlus listed nothing, then <drive>/homebrew for
// each drive, once per drive. A drive ShadowMountPlus has no folder for gets
// its mount's numbers (space->drive_space), or total 0 (free space unknown).
void merge_destinations(StorageSpace *space);

// The drives NASBridge sees mounted itself: a drive's folder belongs to
// another device than /mnt. False when it can't tell (stat refused).
bool probe_drives(std::vector<std::string> *drives);

// Keeps asking (ShadowMountPlus, then NASBridge itself) on its own thread,
// every `seconds`, for the free space, the destinations and the game list. The thread ends on its own after the watcher is gone, so
// nothing is left behind and destroying the watcher never waits.
class StorageWatcher
{
  public:
    explicit StorageWatcher(float seconds = 10.0f);
    ~StorageWatcher();
    StorageWatcher(const StorageWatcher &) = delete;
    StorageWatcher &operator=(const StorageWatcher &) = delete;

    // The latest answer (asked == false until there is one). Every frame is fine.
    StorageSpace latest() const;
    // Asks again now (after a copy or a delete).
    void refresh();

    struct Shared; // what the watcher and its thread share (storage.cpp)

  private:
    std::shared_ptr<Shared> shared_;
};

} // namespace hui::concepts
