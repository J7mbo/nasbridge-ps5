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

struct StorageSpace
{
    bool asked = false;      // false until the first answer arrives
    bool known = false;
    std::uint64_t free = 0;  // bytes
    std::uint64_t total = 0; // bytes (0 when unknown)
    std::string how;         // which way answered, or (unknown) why each way failed
    std::vector<Destination> destinations; // where games can be copied (from ShadowMountPlus)
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
// reply, or why there is none in space->how.
StorageSpace storage_from_json(const std::string &json);

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
