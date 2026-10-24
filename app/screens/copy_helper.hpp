// NASBridge - Copying a file through the copy helper payload.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NASBridge runs in the game sandbox, where a long write to a drive collapses
// to a few MB/s. So each file is handed to the copy helper payload
// (tools/copier/copier.c, shipped as assets/copier.elf), which runs with the
// jailbreak's privileges outside the sandbox, as ps5upload's receiver does.
// The first copy sends it to the PS5's own ELF loader; it then serves every
// file until it has been idle a while. Blocks: call from a worker thread.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::concepts
{

struct HelperCopy
{
    enum class End
    {
        ok,
        stopped,     // stopping() said so
        unavailable, // the helper couldn't be started or reached: copy in-app
        unsupported, // the NAS answers in a way the helper doesn't take (chunked): copy in-app
        lost,        // the helper went away mid-file: try again
        network,     // the NAS couldn't be reached, dropped or went quiet: try again
        refused,     // the NAS answered with an error (`status`; SMB: 0, and `why`)
        file,        // the file couldn't be made or opened (`why`)
        write,       // writing to the drive failed (`why`)
        changed,     // the file on the NAS is a different size than listed
    };
    End end = End::unavailable;
    int status = 0;          // the NAS's HTTP status, when it answered
    std::string why;         // the helper's own words, for the message
    std::uint64_t position = 0; // bytes in the file when it ended
};

// Copies `size` bytes into `local` from the NAS at ip:port, sending `request`
// (a whole GET request from build_request, without a Range header: the helper
// adds one to carry on from what's already in the file), over `streams`
// connections (1 or 2: two fetch neighbouring chunks, written in order).
// progress(bytes in the file, ms spent writing, ms elapsed, connections
// bringing data in) comes a few times a second; stopping() is asked as often,
// and true stops the copy at once.
HelperCopy copy_with_helper(const std::string &ip, int port, std::uint64_t size,
                            const std::string &local, const std::string &request, int streams,
                            const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t, int)> &progress,
                            const std::function<bool()> &stopping);

// Whether copies go through the helper at all (tests turn it off and on).
// Many small files in one go (a folder game can be tens of thousands): the
// helper keeps its connections to the NAS open and fetches them on several
// workers, writing each from its start. Files are handed over as it asks for
// them, so the list may be long.
struct BatchFile
{
    std::string local;   // where it goes on the PS5
    std::uint64_t size = 0;
    std::string request; // the GET (keep-alive), sign-in and all; SMB: the path in the share
};

struct HelperBatch
{
    HelperCopy::End end = HelperCopy::End::unavailable; // unsupported: an older helper
    int status = 0;    // the NAS's HTTP status, when it refused
    std::string why;
    long failed = -1;  // which file (index into the list), when one failed
};

// progress(bytes this run, files this run, workers busy, ms spent saving to
// the drive, ms in all; the last two 0 from an older helper).
using BatchProgress =
    std::function<void(std::uint64_t, std::uint64_t, int, std::uint64_t, std::uint64_t)>;

// done(i): file i is in (written and closed). synced(): a save to the drive
// finished.
HelperBatch copy_batch_with_helper(const std::string &ip, int port, int workers,
                                   const std::vector<BatchFile> &files,
                                   const std::function<void(std::size_t)> &done,
                                   const std::function<void()> &synced,
                                   const BatchProgress &progress,
                                   const std::function<bool()> &stopping);

// Over SMB: the helper signs in itself with these.
struct SmbSignIn
{
    std::string share;
    std::string user; // "guest" when none was given
    std::string domain;
    std::string password;
};

// copy_with_helper over SMB: `path` is the file in the share ("PS5/Game/eboot.bin").
// Over `streams` sign-ins of its own (1 to 4), each fetching the next chunk.
HelperCopy copy_smb_with_helper(const std::string &ip, int port, std::uint64_t size,
                                const std::string &local, const SmbSignIn &sign_in,
                                const std::string &path, int streams,
                                const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t, int)> &progress,
                                const std::function<bool()> &stopping);

// copy_batch_with_helper over SMB: each file's `request` is its path in the share.
HelperBatch copy_smb_batch_with_helper(const std::string &ip, int port, int workers,
                                       const SmbSignIn &sign_in, const std::vector<BatchFile> &files,
                                       const std::function<void(std::size_t)> &done,
                                       const std::function<void()> &synced,
                                       const BatchProgress &progress,
                                       const std::function<bool()> &stopping);

// A partition's type, from its disk's partition table (NASBridge can't read
// a disk from its sandbox; the helper can): "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
// (GPT, lower case) or "mbr:ef". `device` is the partition ("/dev/da1p1").
// "" with *why when it can't be read. Starts the helper if needed: blocks.
std::string partition_type(const std::string &device, std::string *why);

// One step on a drive's files, done by the helper: NASBridge can't see a USB
// or extended drive from its sandbox. `step` is "mkdir", "stat", "rename"
// (to `to`), "unlink" or "rmdir". 0, or the errno it failed with (EIO when
// the helper can't be reached, ENOSYS from an older one). stat fills *size
// and *folder. Starts the helper if needed: blocks.
int helper_file_step(const char *step, const std::string &path, const std::string &to = "",
                     std::uint64_t *size = nullptr, bool *folder = nullptr);

bool copy_helper_enabled();

// Tests: the helper file ("" turns the helper off), the loader's port and the
// helper's port.
void set_copy_helper(const std::string &elf_path, int loader_port, int helper_port);

// How many connections the helper uses for a file (1 or 2; default 2).
int helper_streams();
void set_helper_streams(int streams);
// How many small files a batch fetches at once (1..16): each costs a seek on
// the NAS and a file creation on the PS5, and those waits overlap.
int batch_workers();
void set_batch_workers(int workers);

} // namespace hui::concepts
