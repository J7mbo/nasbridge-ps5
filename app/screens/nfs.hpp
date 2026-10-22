// NASBridge - NFS requests to the NAS, for background threads.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built on libnfs (LGPL-2.1-or-later, pinned in the Dockerfile): NFSv3 over
// TCP. NFSv3 has no sign-in: the NAS lets the PS5 in by its address, and the
// client states the user and group IDs it reads as (kNfsUid and kNfsGid). One
// NfsSession is one connection to one export, used by one thread only; every
// call blocks, so call them from a worker thread. IPv4 addresses only, like
// the other clients.
//
// The games folder is a path on the NAS ("/mnt/user/Games/PS5"); the export
// it's in is the longest of the NAS's exports it starts with
// ("/mnt/user/Games"). Above the exports, listings show the folders that lead
// to them, so the folder picker can browse down to one.

#pragma once

#include "concepts/connection.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct nfs_context;

namespace hui::concepts
{

// Who NASBridge reads as: Unraid's nobody and users, which most home NAS
// exports either map every client to or let read the games anyway.
constexpr int kNfsUid = 99;
constexpr int kNfsGid = 100;

// What went wrong, in terms the Connection screen can explain.
struct NfsFailure
{
    enum class Kind
    {
        none,
        address,     // the host isn't an IPv4 address
        connect,     // the NAS couldn't be reached
        refused,     // nothing answers: NFS is off (or its portmapper, port 111)
        timeout,     // the NAS stopped answering
        no_export,   // the folder isn't in any of the NAS's exports
        not_allowed, // the NAS won't let this PS5 use the export
        not_found,   // no folder or file by that name in the export
        denied,      // the export is open to the PS5, but this file isn't readable
        other,
    };
    Kind kind = Kind::none;
    std::string detail; // libnfs's own words, for the details line
};

// One entry of a folder listing.
struct NfsEntry
{
    std::string name;
    bool folder = false;
    std::uint64_t size = 0;     // files only
    std::uint64_t modified = 0; // seconds since 1970 (UTC)
};

// Where a path is: in which export, and where in it ("/" for its top).
struct NfsLocation
{
    std::string exported; // "" when it's in none
    std::string path;
};
// The longest export `path` is in or is ("/mnt/user/Games" for
// "/mnt/user/Games/PS5").
NfsLocation nfs_location(const std::vector<std::string> &exports, const std::string &path);

// Names a listing leaves out: hidden and NAS-internal entries, as the other
// listings do.
bool nfs_skipped_name(const std::string &name);

// A failure in a few words, for a note or a copy's message: "not found".
std::string nfs_failure_brief(const NfsFailure &failure);

// What a failure means for the player: a headline and what to check.
void describe_nfs_failure(const Connection &connection, const NfsFailure &failure,
                          std::string *headline, std::string *detail);

// Whether this process can connect from a privileged port (below 1024): many
// NAS exports only take those unless set to "insecure". libnfs tries one by
// itself; this says whether it can. Asked once, then remembered.
bool nfs_privileged_ports();

// Tests: the NAS's MOUNT service port, so no portmapper is needed (0: ask the
// portmapper, as on a real NAS).
void set_nfs_mount_port(int port);
int nfs_mount_port();

class NfsSession
{
  public:
    // seconds: how long the NAS may take to answer any one request.
    explicit NfsSession(const Connection &connection, int seconds = 10);
    ~NfsSession();
    NfsSession(const NfsSession &) = delete;
    NfsSession &operator=(const NfsSession &) = delete;

    // The NAS's exports, sorted (asked once per session).
    bool list_exports(std::vector<std::string> *exports, NfsFailure *why);
    // Where `path` is, among the exports. False (no_export) when it's in none.
    bool locate(const std::string &path, NfsLocation *location, NfsFailure *why);
    // A folder's entries ("/mnt/user/Games/PS5/"), without nfs_skipped_name
    // ones. Above the exports, the folders leading to them.
    bool list(const std::string &folder, std::vector<NfsEntry> *entries, NfsFailure *why);
    // Exactly `length` bytes at `offset` of a file.
    bool read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                    std::string *out, NfsFailure *why);
    // A whole small file, at most `max` bytes (*too_big, and false, when
    // it's longer).
    bool read_file(const std::string &file, std::size_t max, std::string *out, bool *too_big,
                   NfsFailure *why);
    // A file from `offset` to its end, handed to `sink` a piece at a time.
    // *size is the file's size, known before the first piece. `sink` returns
    // false to stop: the call then returns false with why->kind none.
    bool stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                const std::function<bool(const char *data, std::size_t length)> &sink,
                NfsFailure *why);
    // Every folder under `folder`, and it, listed over up to `lanes`
    // connections (this one and others made for the walk) with up to
    // `at_once` listings in flight on each. `found` gets each folder's path
    // under `folder` ("" for it, "sub/" for one in it) and its entries, in no
    // set order, and returns false to stop: the call then returns false with
    // why->kind none.
    bool walk(const std::string &folder, int lanes, int at_once,
              const std::function<bool(const std::string &inside,
                                       const std::vector<NfsEntry> &entries)> &found,
              NfsFailure *why);

  private:
    nfs_context *connect(const std::string &exported, NfsFailure *why);
    bool attach(const std::string &exported, NfsFailure *why);
    void detach();

    Connection connection_;
    int seconds_;
    bool have_exports_ = false;
    std::vector<std::string> exports_;
    nfs_context *nfs_ = nullptr;
    std::string exported_; // the export nfs_ has mounted
};

} // namespace hui::concepts
