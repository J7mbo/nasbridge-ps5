// NASBridge - SMB requests to the NAS, for background threads.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built on libsmb2 (LGPL-2.1-or-later, pinned in the Dockerfile): SMB 2 and 3,
// signed in with a user name and password (NTLM), or as a guest. One
// SmbSession is one sign-in to the NAS, used by one thread only; every call
// blocks, so call them from a worker thread. IPv4 addresses only, like the
// WebDAV client.
//
// The games folder's first part is the share: "/Games/PS5" is the folder PS5
// in the share Games. Signing in needs only the address, user name and
// password, so the folder picker can start from the NAS's list of shares.

#pragma once

#include "concepts/connection.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct smb2_context;

namespace hui::concepts
{

// What went wrong, in terms the Connection screen can explain.
struct SmbFailure
{
    enum class Kind
    {
        none,
        address,   // the host isn't an IPv4 address
        connect,   // the NAS couldn't be reached (no route, nothing at that address)
        refused,   // nothing answers on that port: SMB is off, or another port
        timeout,   // the NAS stopped answering
        sign_in,   // wrong user name or password, or the account can't sign in
        no_share,  // no share by that name
        not_found, // no folder or file by that name in the share
        denied,    // signed in, but this user may not read it
        protocol,  // the NAS doesn't speak SMB 2 or 3 (only the old SMB 1)
        other,
    };
    Kind kind = Kind::none;
    std::uint32_t status = 0; // the NAS's NT status, when it gave one
    std::string detail;       // libsmb2's own words, for the details line
};

// One entry of a folder listing.
struct SmbEntry
{
    std::string name;
    bool folder = false;
    std::uint64_t size = 0;     // files only
    std::uint64_t modified = 0; // seconds since 1970 (UTC)
};

// Where a folder setting points: "/Games/PS5/" is share "Games", path "PS5".
// path has no slash at either end and is "" for the top of the share; share
// is "" for "/" (the NAS itself, where the shares are listed).
struct SmbLocation
{
    std::string share;
    std::string path;
};
SmbLocation smb_location(const std::string &folder);

// "WORKGROUP\james" is user "james" in domain "WORKGROUP"; a plain name has
// no domain.
void split_smb_user(const std::string &text, std::string *domain, std::string *user);

// Names a listing leaves out: hidden and NAS-internal entries, as the WebDAV
// listing does, plus Windows' own ("$RECYCLE.BIN", "System Volume Information").
bool smb_skipped_name(const std::string &name);

// A failure in a few words, for a note or a copy's message: "not found",
// "access denied".
std::string smb_failure_brief(const SmbFailure &failure);

// What a failure means for the player: a headline and what to check.
void describe_smb_failure(const Connection &connection, const SmbFailure &failure,
                          std::string *headline, std::string *detail);

class SmbSession
{
  public:
    // seconds: how long the NAS may take to answer any one request.
    explicit SmbSession(const Connection &connection, int seconds = 10);
    ~SmbSession();
    SmbSession(const SmbSession &) = delete;
    SmbSession &operator=(const SmbSession &) = delete;

    // The NAS's shares that hold files (no printers, no hidden "x$" shares),
    // sorted by name.
    bool list_shares(std::vector<std::string> *names, SmbFailure *why);
    // A folder's entries ("/Games/PS5/"), without smb_skipped_name ones. For
    // "/", the shares, as folders.
    bool list(const std::string &folder, std::vector<SmbEntry> *entries, SmbFailure *why);
    // Exactly `length` bytes at `offset` of a file ("/Games/PS5/game.exfat").
    bool read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                    std::string *out, SmbFailure *why);
    // A whole small file, at most `max` bytes (*too_big, and false, when
    // it's longer).
    bool read_file(const std::string &file, std::size_t max, std::string *out, bool *too_big,
                   SmbFailure *why);
    // A file from `offset` to its end, handed to `sink` a piece at a time (a
    // big file needs only a little memory). *size is the file's size, known
    // before the first piece. `sink` returns false to stop: the call then
    // returns false with why->kind none.
    bool stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                const std::function<bool(const char *data, std::size_t length)> &sink,
                SmbFailure *why);
    // Every folder under `folder` ("/Games/PS5/Game/"), and it, listed over
    // up to `sign_ins` sign-ins (this one and others made for the walk) with
    // up to `at_once` listings in flight on each: a NAS slow to read its
    // disks, or slow to answer over the network, is then slow once per batch
    // rather than once per folder. `found` gets each folder's path under
    // `folder` ("" for it, "sub/" for one in it) and its entries, in no set
    // order, and returns false to stop: the call then returns false with
    // why->kind none.
    bool walk(const std::string &folder, int sign_ins, int at_once,
              const std::function<bool(const std::string &inside,
                                       const std::vector<SmbEntry> &entries)> &found,
              SmbFailure *why);

  private:
    smb2_context *connect(const std::string &share, SmbFailure *why);
    bool attach(const std::string &share, SmbFailure *why);
    void detach();
    void failed(int rc, SmbFailure *why);
    void failed(smb2_context *smb2, int rc, SmbFailure *why);

    Connection connection_;
    int seconds_;
    smb2_context *smb2_ = nullptr;
    std::string share_; // the share smb2_ is connected to ("IPC$" while listing shares)
};

} // namespace hui::concepts
