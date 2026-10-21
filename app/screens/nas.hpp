// NASBridge - Reading the NAS over WebDAV or SMB, whichever the connection uses.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The library and the copy queue read the NAS through this, so they don't
// care which way it's shared. Paths are the same either way: the games folder
// setting plus what's under it ("/Games/PS5/Game/sce_sys/"); for SMB the first
// part is the share. Listings come back as DavEntry for both, with a folder's
// entries' paths decoded from the root.
//
// One NasReader belongs to one thread: over SMB it keeps one sign-in open for
// all its requests (signing in costs several round trips). Every call blocks.

#pragma once

#include "concepts/connection.hpp"
#include "concepts/http.hpp"
#include "concepts/smb.hpp"
#include "concepts/nas_probe.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hui::concepts
{

// Why a request failed.
struct NasProblem
{
    HttpReply reply;   // WebDAV: the reply (connected false: it never connected)
    SmbFailure smb;    // SMB: what went wrong
    std::string brief; // a few words for a note: "not found (HTTP 404)", "access denied"
    bool reached = false; // the NAS answered (it refused, rather than couldn't be reached)
};

class NasReader
{
  public:
    // seconds: how long the NAS may take to answer a request (SMB).
    explicit NasReader(const Connection &connection, int seconds = 15);
    ~NasReader();
    NasReader(const NasReader &) = delete;
    NasReader &operator=(const NasReader &) = delete;

    bool smb() const
    {
        return connection_.type == ConnectionType::smb;
    }

    // A folder's entries ("/Games/PS5/"), without hidden and NAS-internal ones.
    bool list(const std::string &folder, std::vector<DavEntry> *entries,
              NasProblem *problem = nullptr);
    // A whole small file, at most `max` bytes.
    bool read_file(const std::string &path, std::size_t max, std::string *out,
                   NasProblem *problem = nullptr);
    // Exactly `length` bytes at `offset` of a file.
    bool read_range(const std::string &path, std::uint64_t offset, std::size_t length,
                    std::string *out, std::string *why);
    // SMB only: every folder under `folder`, and it, several listed at once
    // (SmbSession::walk). `found` gets each folder ("/Games/PS5/Game/sub/")
    // and its listing, and returns false to stop (false, problem->brief "").
    bool walk_smb(const std::string &folder,
                  const std::function<bool(const std::string &folder,
                                           const std::vector<DavEntry> &entries)> &found,
                  NasProblem *problem = nullptr);
    // The SMB sign-in itself (SMB only), for streaming a file.
    SmbSession &session();

  private:
    Connection connection_;
    int seconds_;
    std::unique_ptr<SmbSession> session_;
};

// A games folder listing that failed, in Test connection's words.
ProbeResult explain_nas_failure(const Connection &connection, const NasProblem &problem);

// SMB entries as a listing of `folder` (the same paths a WebDAV listing has).
std::vector<DavEntry> smb_as_listing(const std::string &folder, const std::vector<SmbEntry> &entries);

} // namespace hui::concepts
