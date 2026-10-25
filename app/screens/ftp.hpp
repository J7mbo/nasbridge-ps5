// NASBridge - FTP requests to the NAS, for background threads.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plain FTP (RFC 959) with the extensions NAS servers have: passive data
// connections (EPSV, else PASV), binary transfers, SIZE, and REST to start a
// file part way. Folders are listed with MLSD (RFC 3659) where the server has
// it, and with LIST's "ls -l" lines where it doesn't (vsftpd, which Unraid
// uses). No FTPS: a NAS that insists on it is explained. One FtpSession is
// one sign-in (a control connection), used by one thread only; every call
// blocks, so call them from a worker thread. IPv4 addresses only, like the
// WebDAV client.
//
// Paths are the NAS's own, as the signed-in user sees them: "/Games/PS5/".
// An empty user name signs in as "anonymous".

#pragma once

#include "concepts/connection.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::concepts
{

// What went wrong, in terms the Connection screen can explain.
struct FtpFailure
{
    enum class Kind
    {
        none,
        address,   // the host isn't an IPv4 address
        connect,   // the NAS couldn't be reached (no route, nothing at that address)
        refused,   // nothing answers on that port: FTP is off, or another port
        timeout,   // the NAS stopped answering
        lost,      // the NAS closed the connection
        sign_in,   // wrong user name or password, or the account can't sign in
        busy,      // the NAS turned the connection away: too many open
        tls,       // the NAS only takes FTPS (encrypted FTP)
        not_found, // no folder or file by that name
        denied,    // signed in, but this user may not read it
        passive,   // signed in, but no data connection could be opened
        other,
    };
    Kind kind = Kind::none;
    int code = 0;       // the NAS's reply code, when it gave one
    std::string detail; // the NAS's own words, for the details line
};

// One entry of a folder listing.
struct FtpEntry
{
    std::string name;
    bool folder = false;
    std::uint64_t size = 0;   // files only
    std::string modified;     // MLSD: seconds since 1970 (UTC); LIST: the date as listed
};

// The user name and password FtpSession and the copy helper sign in with:
// "anonymous" (and an e-mail-like password, as servers expect) when no user
// name is set.
std::string ftp_user(const Connection &connection);
std::string ftp_password(const Connection &connection);

// A folder or file setting as an FTP path: "/Games/PS5/" is "/Games/PS5", "" is "/".
std::string ftp_path(const std::string &path);

// One line of a listing; false for lines that aren't an entry (totals,
// ".", "..", links). MLSD: "type=file;size=12;modify=20261025081500; name".
// LIST: "-rw-r--r--   1 user group 12 Oct 25 08:15 name", or IIS's
// "10-25-26  08:15AM  12 name".
bool parse_mlsd_line(const std::string &line, FtpEntry *entry);
bool parse_list_line(const std::string &line, FtpEntry *entry);

// Names a listing leaves out: the same as over SMB (hidden, NAS-internal).
bool ftp_skipped_name(const std::string &name);

// A failure in a few words, for a note or a copy's message: "not found",
// "access denied".
std::string ftp_failure_brief(const FtpFailure &failure);

// What a failure means for the player: a headline and what to check.
void describe_ftp_failure(const Connection &connection, const FtpFailure &failure,
                          std::string *headline, std::string *detail);

class FtpSession
{
  public:
    // seconds: how long the NAS may take to answer any one request.
    explicit FtpSession(const Connection &connection, int seconds = 10);
    ~FtpSession();
    FtpSession(const FtpSession &) = delete;
    FtpSession &operator=(const FtpSession &) = delete;

    // Signs in, if not already. A NAS that turns the connection away (too
    // many) is asked again, up to `tries` times in all, a little later.
    bool sign_in(FtpFailure *why, int tries = 3);
    // A folder's entries ("/Games/PS5/"), without ftp_skipped_name ones.
    bool list(const std::string &folder, std::vector<FtpEntry> *entries, FtpFailure *why);
    // Exactly `length` bytes at `offset` of a file. A little more is read and
    // kept, so a run of small reads close together costs one transfer.
    bool read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                    std::string *out, FtpFailure *why);
    // A whole small file, at most `max` bytes (*too_big, and false, when
    // it's longer).
    bool read_file(const std::string &file, std::size_t max, std::string *out, bool *too_big,
                   FtpFailure *why);
    // A file from `offset` to its end, handed to `sink` a piece at a time.
    // *size (if given) is the file's size, set before the first piece when
    // the NAS says it (SIZE) and left as it was when it doesn't. `sink`
    // returns false to stop: the call then returns false with why->kind none.
    bool stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                const std::function<bool(const char *data, std::size_t length)> &sink,
                FtpFailure *why);
    // Every folder under `folder` ("/Games/PS5/Game/"), and it, listed over
    // up to `connections` sign-ins at once (this one and others made for the
    // walk; one the NAS turns away just isn't used): an FTP connection lists
    // one folder at a time, so a NAS slow to read its disks or slow to answer
    // is then slow once per batch rather than once per folder. `found` gets
    // each folder's path under `folder` ("" for it, "sub/" for one in it) and
    // its entries, in no set order, on this thread, and returns false to
    // stop: the call then returns false with why->kind none.
    bool walk(const std::string &folder, int connections,
              const std::function<bool(const std::string &inside,
                                       const std::vector<FtpEntry> &entries)> &found,
              FtpFailure *why);
    // From another thread: cuts the connections off, so a call in progress
    // ends at once (failing). The session signs in again on its next call.
    void cut();

  private:
    bool sign_in_once(FtpFailure *why);
    void disconnect();
    bool read_line(std::string *line, FtpFailure *why);
    bool reply(int *code, std::string *text, FtpFailure *why);
    bool command(const std::string &line, int *code, std::string *text, FtpFailure *why);
    int open_data(FtpFailure *why);
    int begin(const std::string &line, std::uint64_t offset, bool *finished, FtpFailure *why);
    bool finish(int data, FtpFailure *why);
    void abandon(int data);
    void refused(int code, const std::string &text, FtpFailure *why);
    void network(int error, FtpFailure *why);

    Connection connection_;
    int seconds_;
    std::atomic<int> control_{-1};
    std::atomic<int> data_{-1}; // the transfer in progress, for cut()
    std::string pending_;       // read from the control connection, past the last line
    bool mlsd_ = false;         // the NAS lists with MLSD
    bool epsv_ = true;          // ... and takes EPSV (until it refuses it)
    std::string ahead_path_;    // read_range's last read, kept
    std::uint64_t ahead_at_ = 0;
    std::string ahead_;
};

} // namespace hui::concepts
