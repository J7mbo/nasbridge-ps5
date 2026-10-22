// NASBridge - SMB requests to the NAS.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/smb.hpp"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
#include <smb2/smb2-errors.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

namespace hui::concepts
{

namespace
{

// "a/b" from any mix of slashes, or "" for none.
std::string trimmed_path(const std::string &text)
{
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && (text[start] == '/' || text[start] == '\\'))
        ++start;
    while (end > start && (text[end - 1] == '/' || text[end - 1] == '\\'))
        --end;
    return text.substr(start, end - start);
}

std::string lowered_ascii(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// libsmb2 reports an error as a negative errno; the NT status, when the NAS
// sent one, says more.
SmbFailure::Kind kind_of(std::uint32_t status, int error)
{
    switch (status)
    {
    case SMB2_STATUS_LOGON_FAILURE:
    case SMB2_STATUS_WRONG_PASSWORD:
    case SMB2_STATUS_WRONG_PASSWORD_CORE:
    case SMB2_STATUS_NO_SUCH_USER:
    case SMB2_STATUS_ACCOUNT_RESTRICTION:
    case SMB2_STATUS_INVALID_LOGON_HOURS:
    case SMB2_STATUS_PASSWORD_EXPIRED:
    case SMB2_STATUS_PASSWORD_MUST_CHANGE:
    case SMB2_STATUS_ACCOUNT_DISABLED:
    case SMB2_STATUS_ACCOUNT_EXPIRED:
    case SMB2_STATUS_ACCOUNT_LOCKED_OUT:
        return SmbFailure::Kind::sign_in;
    case SMB2_STATUS_BAD_NETWORK_NAME:
        return SmbFailure::Kind::no_share;
    case SMB2_STATUS_OBJECT_NAME_NOT_FOUND:
    case SMB2_STATUS_OBJECT_PATH_NOT_FOUND:
    case SMB2_STATUS_NO_SUCH_FILE:
    case SMB2_STATUS_NOT_A_DIRECTORY:
        return SmbFailure::Kind::not_found;
    case SMB2_STATUS_ACCESS_DENIED:
    case SMB2_STATUS_NETWORK_ACCESS_DENIED:
        return SmbFailure::Kind::denied;
    case SMB2_STATUS_NOT_SUPPORTED:
        return SmbFailure::Kind::protocol;
    default:
        break;
    }
    switch (error)
    {
    case ECONNREFUSED:
        return SmbFailure::Kind::refused;
    case ETIMEDOUT:
        return SmbFailure::Kind::timeout;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EHOSTDOWN:
    case ENETDOWN:
        return SmbFailure::Kind::connect;
    case ECONNRESET:
    case EPIPE:
        return SmbFailure::Kind::protocol; // usually an SMB 1-only NAS hanging up
    default:
        return SmbFailure::Kind::other;
    }
}

} // namespace

SmbLocation smb_location(const std::string &folder)
{
    const std::string path = trimmed_path(folder);
    SmbLocation location;
    const std::size_t slash = path.find_first_of("/\\");
    if (slash == std::string::npos)
    {
        location.share = path;
        return location;
    }
    location.share = path.substr(0, slash);
    location.path = trimmed_path(path.substr(slash + 1));
    std::replace(location.path.begin(), location.path.end(), '\\', '/');
    return location;
}

void split_smb_user(const std::string &text, std::string *domain, std::string *user)
{
    const std::size_t slash = text.find('\\');
    if (slash == std::string::npos)
    {
        domain->clear();
        *user = text;
        return;
    }
    *domain = text.substr(0, slash);
    *user = text.substr(slash + 1);
}

bool smb_skipped_name(const std::string &name)
{
    if (name.empty() || name[0] == '.')
        return true;
    const std::string lower = lowered_ascii(name);
    return lower == "@eadir" || lower == "#recycle" || lower == "$recycle.bin" ||
           lower == "system volume information" || lower == "#snapshot";
}

std::string smb_failure_brief(const SmbFailure &failure)
{
    switch (failure.kind)
    {
    case SmbFailure::Kind::none:
        return "no problem";
    case SmbFailure::Kind::address:
        return "not an IP address";
    case SmbFailure::Kind::connect:
        return "couldn't reach the NAS";
    case SmbFailure::Kind::refused:
        return "SMB isn't answering";
    case SmbFailure::Kind::timeout:
        return "the NAS stopped answering";
    case SmbFailure::Kind::sign_in:
        return "sign-in refused";
    case SmbFailure::Kind::no_share:
        return "no such share";
    case SmbFailure::Kind::not_found:
        return "not found";
    case SmbFailure::Kind::denied:
        return "access denied";
    case SmbFailure::Kind::protocol:
        return "the NAS doesn't speak SMB 2 or 3";
    case SmbFailure::Kind::other:
        break;
    }
    return failure.detail.empty() ? std::string("SMB error") : failure.detail;
}

void describe_smb_failure(const Connection &connection, const SmbFailure &failure,
                          std::string *headline, std::string *detail)
{
    const std::string where = connection.host + ":" + std::to_string(connection.effective_port());
    switch (failure.kind)
    {
    case SmbFailure::Kind::none:
        *headline = "Connected";
        detail->clear();
        return;
    case SmbFailure::Kind::address:
        *headline = "Enter the NAS's IP address";
        *detail = "NASBridge can't look up names yet: use the NAS's IPv4 address, "
                  "such as 192.168.1.20.";
        return;
    case SmbFailure::Kind::connect:
        *headline = "Can't reach the NAS";
        *detail = "Nothing answered at " + connection.host +
                  ". Check the address, and that the NAS and the PS5 are on the same network.";
        return;
    case SmbFailure::Kind::refused:
        *headline = "SMB isn't answering";
        *detail = "The NAS is there, but nothing answers on port " +
                  std::to_string(connection.effective_port()) +
                  ". Turn on SMB in the NAS's settings, "
                  "or check the port (usually 445).";
        return;
    case SmbFailure::Kind::timeout:
        *headline = "The NAS stopped answering";
        *detail = "No reply from " + where + " in time. Check the NAS is awake and try again.";
        return;
    case SmbFailure::Kind::sign_in:
        *headline = "Wrong user name or password";
        *detail = "The NAS didn't accept the sign-in. Check both, or leave them empty "
                  "for guest access if the NAS allows it. A domain goes before the "
                  "name: DOMAIN\\name.";
        return;
    case SmbFailure::Kind::no_share:
        *headline = "No such share";
        *detail = "The NAS has no share called \"" + smb_location(connection.folder).share +
                  "\". The folder's first part is the share: pick it with the folder button.";
        return;
    case SmbFailure::Kind::not_found:
        *headline = "Folder not found";
        *detail = "The share has no folder " + connection.folder +
                  ". Pick the games folder with the folder button.";
        return;
    case SmbFailure::Kind::denied:
        *headline = "Not allowed to read this folder";
        *detail = "The NAS accepted the sign-in, but this user may not read " +
                  connection.folder + ". Give the user read access to the share.";
        return;
    case SmbFailure::Kind::protocol:
        *headline = "The NAS doesn't speak SMB 2 or 3";
        *detail = "NASBridge needs SMB 2 or newer. In the NAS's SMB settings, set the "
                  "minimum protocol to SMB2 (SMB 1 is old and unsafe).";
        return;
    case SmbFailure::Kind::other:
        break;
    }
    *headline = "Couldn't read the NAS";
    *detail = failure.detail.empty() ? std::string("Something went wrong talking to ") + where + "."
                                     : failure.detail;
}

SmbSession::SmbSession(const Connection &connection, int seconds)
    : connection_(connection), seconds_(seconds)
{
}

SmbSession::~SmbSession()
{
    detach();
}

void SmbSession::detach()
{
    if (smb2_ == nullptr)
        return;
    smb2_disconnect_share(smb2_);
    smb2_destroy_context(smb2_);
    smb2_ = nullptr;
    share_.clear();
}

void SmbSession::failed(int rc, SmbFailure *why)
{
    failed(smb2_, rc, why);
}

void SmbSession::failed(smb2_context *smb2, int rc, SmbFailure *why)
{
    why->status = smb2 != nullptr ? static_cast<std::uint32_t>(smb2_get_nterror(smb2)) : 0;
    const char *text = smb2 != nullptr ? smb2_get_error(smb2) : nullptr;
    why->detail = text != nullptr ? text : "";
    while (!why->detail.empty() && (why->detail.back() == '\n' || why->detail.back() == ' '))
        why->detail.pop_back();
    int error = rc < 0 ? -rc : rc;
    // A failed connect only shows up in the message ("Socket connect failed
    // with 111"), and an expired connect timeout has no errno at all.
    const std::size_t connect_at = why->detail.find("connect failed with ");
    if (connect_at != std::string::npos)
        error = std::atoi(why->detail.c_str() + connect_at + 20);
    why->kind = kind_of(why->status, error);
    if (why->kind == SmbFailure::Kind::other && why->detail.find("Timeout") != std::string::npos)
        why->kind = SmbFailure::Kind::timeout;
}

smb2_context *SmbSession::connect(const std::string &share, SmbFailure *why)
{
    *why = SmbFailure{};
    in_addr address{};
    if (::inet_pton(AF_INET, connection_.host.c_str(), &address) != 1)
    {
        why->kind = SmbFailure::Kind::address;
        return nullptr;
    }
    smb2_context *smb2 = smb2_init_context();
    if (smb2 == nullptr)
    {
        why->kind = SmbFailure::Kind::other;
        why->detail = "Out of memory";
        return nullptr;
    }
    smb2_set_timeout(smb2, seconds_);
    smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    std::string domain, user;
    split_smb_user(connection_.username, &domain, &user);
    if (user.empty())
        user = "guest";
    if (!domain.empty())
        smb2_set_domain(smb2, domain.c_str());
    smb2_set_password(smb2, connection_.password.c_str());
    const std::string server = connection_.host + ":" + std::to_string(connection_.effective_port());
    const int rc = smb2_connect_share(smb2, server.c_str(), share.c_str(), user.c_str());
    if (rc < 0)
    {
        failed(smb2, rc, why);
        smb2_destroy_context(smb2);
        return nullptr;
    }
    return smb2;
}

bool SmbSession::attach(const std::string &share, SmbFailure *why)
{
    *why = SmbFailure{};
    if (smb2_ != nullptr && share_ == share)
        return true;
    detach();
    smb2_ = connect(share, why);
    if (smb2_ == nullptr)
        return false;
    share_ = share;
    return true;
}

namespace
{

struct ShareEnum
{
    bool finished = false;
    int status = 0;
    std::vector<std::string> names;
};

void share_enum_done(smb2_context *smb2, int status, void *command_data, void *private_data)
{
    ShareEnum *state = static_cast<ShareEnum *>(private_data);
    state->finished = true;
    state->status = status;
    if (status != 0 || command_data == nullptr)
        return;
    srvsvc_NetrShareEnum_rep *reply = static_cast<srvsvc_NetrShareEnum_rep *>(command_data);
    const srvsvc_SHARE_INFO_1_CONTAINER &level1 = reply->ses.ShareInfo.Level1;
    for (std::uint32_t i = 0; i < level1.EntriesRead && level1.Buffer != nullptr; ++i)
    {
        const srvsvc_SHARE_INFO_1 &share = level1.Buffer->share_info_1[i];
        if (share.netname.utf8 == nullptr)
            continue;
        const std::string name = share.netname.utf8;
        // Disk shares only, and not the hidden ones ("C$", "IPC$", "print$").
        if ((share.type & 3) != SHARE_TYPE_DISKTREE || (share.type & SHARE_TYPE_HIDDEN) ||
            name.empty() || name.back() == '$')
            continue;
        state->names.push_back(name);
    }
    smb2_free_data(smb2, reply);
}

} // namespace

bool SmbSession::list_shares(std::vector<std::string> *names, SmbFailure *why)
{
    names->clear();
    if (!attach("IPC$", why))
        return false;
    ShareEnum state;
    int rc = smb2_share_enum_async(smb2_, SHARE_INFO_1, share_enum_done, &state);
    const std::time_t deadline = std::time(nullptr) + seconds_ + 1;
    while (rc == 0 && !state.finished)
    {
        pollfd pfd{};
        pfd.fd = smb2_get_fd(smb2_);
        pfd.events = static_cast<short>(smb2_which_events(smb2_));
        if (::poll(&pfd, 1, 250) < 0 && errno != EINTR)
        {
            rc = -errno;
            break;
        }
        if (pfd.revents != 0 && smb2_service(smb2_, pfd.revents) < 0)
        {
            rc = -EIO;
            break;
        }
        if (std::time(nullptr) > deadline)
        {
            rc = -ETIMEDOUT;
            break;
        }
    }
    if (rc == 0)
        rc = state.status;
    if (rc != 0)
    {
        failed(rc, why);
        detach(); // the connection may be mid-reply: start again next time
        return false;
    }
    *names = std::move(state.names);
    std::sort(names->begin(), names->end(),
              [](const std::string &a, const std::string &b)
              { return lowered_ascii(a) < lowered_ascii(b); });
    return true;
}

bool SmbSession::list(const std::string &folder, std::vector<SmbEntry> *entries, SmbFailure *why)
{
    entries->clear();
    const SmbLocation location = smb_location(folder);
    if (location.share.empty())
    {
        std::vector<std::string> shares;
        if (!list_shares(&shares, why))
            return false;
        for (std::string &name : shares)
        {
            SmbEntry entry;
            entry.name = std::move(name);
            entry.folder = true;
            entries->push_back(std::move(entry));
        }
        return true;
    }
    if (!attach(location.share, why))
        return false;
    smb2dir *dir = smb2_opendir(smb2_, location.path.c_str());
    if (dir == nullptr)
    {
        failed(-ENOENT, why);
        return false;
    }
    while (const smb2dirent *item = smb2_readdir(smb2_, dir))
    {
        if (item->name == nullptr || smb_skipped_name(item->name))
            continue;
        SmbEntry entry;
        entry.name = item->name;
        entry.folder = item->st.smb2_type == SMB2_TYPE_DIRECTORY;
        entry.size = entry.folder ? 0 : item->st.smb2_size;
        entry.modified = item->st.smb2_mtime;
        entries->push_back(std::move(entry));
    }
    smb2_closedir(smb2_, dir);
    return true;
}

bool SmbSession::read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                            std::string *out, SmbFailure *why)
{
    out->clear();
    const SmbLocation location = smb_location(file);
    if (location.share.empty() || location.path.empty())
    {
        *why = SmbFailure{};
        why->kind = SmbFailure::Kind::not_found;
        return false;
    }
    if (!attach(location.share, why))
        return false;
    smb2fh *handle = smb2_open(smb2_, location.path.c_str(), O_RDONLY);
    if (handle == nullptr)
    {
        failed(-ENOENT, why);
        return false;
    }
    out->resize(length);
    const std::uint32_t most = std::max<std::uint32_t>(smb2_get_max_read_size(smb2_), 64u * 1024u);
    std::size_t got = 0;
    bool ok = true;
    while (got < length)
    {
        const std::uint32_t ask = static_cast<std::uint32_t>(std::min<std::size_t>(length - got, most));
        const int rc = smb2_pread(smb2_, handle, reinterpret_cast<std::uint8_t *>(&(*out)[got]), ask,
                                  offset + got);
        if (rc <= 0) // 0: the file ended before `length` bytes
        {
            if (rc < 0)
                failed(rc, why);
            else
            {
                why->kind = SmbFailure::Kind::other;
                why->detail = "The file is shorter than expected";
            }
            ok = false;
            break;
        }
        got += static_cast<std::size_t>(rc);
    }
    smb2_close(smb2_, handle);
    if (!ok)
        out->clear();
    return ok;
}

bool SmbSession::read_file(const std::string &file, std::size_t max, std::string *out,
                           bool *too_big, SmbFailure *why)
{
    *too_big = false;
    out->clear();
    std::string data;
    const bool ok = stream(
        file, 0, nullptr,
        [&](const char *piece, std::size_t length)
        {
            if (data.size() + length > max)
            {
                *too_big = true;
                return false;
            }
            data.append(piece, length);
            return true;
        },
        why);
    if (ok)
        *out = std::move(data);
    return ok;
}

bool SmbSession::stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                        const std::function<bool(const char *data, std::size_t length)> &sink,
                        SmbFailure *why)
{
    const SmbLocation location = smb_location(file);
    if (location.share.empty() || location.path.empty())
    {
        *why = SmbFailure{};
        why->kind = SmbFailure::Kind::not_found;
        return false;
    }
    if (!attach(location.share, why))
        return false;
    smb2fh *handle = smb2_open(smb2_, location.path.c_str(), O_RDONLY);
    if (handle == nullptr)
    {
        failed(-ENOENT, why);
        return false;
    }
    smb2_stat_64 info{};
    int rc = smb2_fstat(smb2_, handle, &info);
    if (rc < 0)
    {
        failed(rc, why);
        smb2_close(smb2_, handle);
        return false;
    }
    if (size != nullptr)
        *size = info.smb2_size;
    // The biggest read the NAS takes, up to 4 MiB (a read is one round trip).
    const std::uint32_t most = std::min<std::uint32_t>(
        std::max<std::uint32_t>(smb2_get_max_read_size(smb2_), 64u * 1024u), 4u << 20);
    std::vector<std::uint8_t> buffer(most);
    std::uint64_t at = offset;
    bool ok = true;
    while (at < info.smb2_size)
    {
        const std::uint32_t ask =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(info.smb2_size - at, most));
        rc = smb2_pread(smb2_, handle, buffer.data(), ask, at);
        if (rc <= 0)
        {
            if (rc < 0)
                failed(rc, why);
            else
            {
                why->kind = SmbFailure::Kind::other;
                why->detail = "The file is shorter than expected";
            }
            ok = false;
            break;
        }
        at += static_cast<std::uint64_t>(rc);
        if (!sink(reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(rc)))
        {
            ok = false;
            break;
        }
    }
    smb2_close(smb2_, handle);
    if (!ok && why->kind != SmbFailure::Kind::none)
        detach(); // a failed read may leave the connection mid-reply
    return ok;
}

namespace
{

struct WalkAsk;

// One walk's shared state: the folders still to list, and the answers.
struct Walk
{
    std::vector<int> in_flight; // per sign-in
    int failure = 0;            // the first failed listing's status
    std::size_t failed_lane = 0;
    std::string failed_path;
    std::deque<std::string> waiting; // paths in the share, "" for its top
    std::vector<WalkAsk *> answered; // listed, not yet handed to `found`
};

struct WalkAsk
{
    Walk *walk;
    std::size_t lane; // which sign-in asked
    std::string path;
    std::vector<SmbEntry> entries;
};

void walk_answer(smb2_context *smb2, int status, void *command_data, void *private_data)
{
    WalkAsk *ask = static_cast<WalkAsk *>(private_data);
    Walk *walk = ask->walk;
    --walk->in_flight[ask->lane];
    if (status != 0 || command_data == nullptr)
    {
        if (walk->failure == 0)
        {
            walk->failure = status != 0 ? status : -EIO;
            walk->failed_lane = ask->lane;
            walk->failed_path = ask->path;
        }
        delete ask;
        return;
    }
    smb2dir *dir = static_cast<smb2dir *>(command_data);
    while (const smb2dirent *item = smb2_readdir(smb2, dir))
    {
        if (item->name == nullptr || smb_skipped_name(item->name))
            continue;
        SmbEntry entry;
        entry.name = item->name;
        entry.folder = item->st.smb2_type == SMB2_TYPE_DIRECTORY;
        entry.size = entry.folder ? 0 : item->st.smb2_size;
        entry.modified = item->st.smb2_mtime;
        ask->entries.push_back(std::move(entry));
    }
    smb2_closedir(smb2, dir);
    walk->answered.push_back(ask);
}

} // namespace

bool SmbSession::walk(
    const std::string &folder, int sign_ins, int at_once,
    const std::function<bool(const std::string &inside, const std::vector<SmbEntry> &entries)> &found,
    SmbFailure *why)
{
    const SmbLocation location = smb_location(folder);
    if (location.share.empty())
    {
        *why = SmbFailure{};
        why->kind = SmbFailure::Kind::not_found;
        return false;
    }
    if (!attach(location.share, why))
        return false;
    sign_ins = std::max(sign_ins, 1);
    at_once = std::max(at_once, 1);
    // Lane 0 is this session's own sign-in; the others are made for the walk
    // and signed out after it. A NAS answers one sign-in's requests one after
    // another, so a NAS that is slow to read its disks is only faster with
    // more sign-ins; several requests in flight on each also hide the network's
    // delay.
    std::vector<smb2_context *> lanes{smb2_};
    Walk walk;
    walk.in_flight.push_back(0);
    walk.waiting.push_back(location.path);
    bool stopped = false;
    int rc = 0;
    std::time_t heard = std::time(nullptr);
    std::vector<pollfd> fds;
    const auto busy = [&]
    {
        for (int n : walk.in_flight)
            if (n > 0)
                return true;
        return false;
    };
    while (rc == 0 && walk.failure == 0 && !stopped && (busy() || !walk.waiting.empty()))
    {
        // More sign-ins once there is more than one folder to list (a small
        // folder never needs them). One that fails just isn't used.
        if (static_cast<int>(lanes.size()) < sign_ins && walk.waiting.size() > 1)
        {
            SmbFailure ignored;
            smb2_context *extra = connect(location.share, &ignored);
            if (extra == nullptr)
                sign_ins = static_cast<int>(lanes.size());
            else
            {
                lanes.push_back(extra);
                walk.in_flight.push_back(0);
            }
        }
        for (std::size_t lane = 0; lane < lanes.size() && rc == 0; ++lane)
            while (walk.in_flight[lane] < at_once && !walk.waiting.empty())
            {
                WalkAsk *ask = new WalkAsk{&walk, lane, std::move(walk.waiting.front()), {}};
                walk.waiting.pop_front();
                rc = smb2_opendir_async(lanes[lane], ask->path.c_str(), walk_answer, ask);
                if (rc < 0)
                {
                    walk.failed_lane = lane;
                    delete ask;
                    break;
                }
                ++walk.in_flight[lane];
            }
        if (rc < 0)
            break;
        fds.assign(lanes.size(), pollfd{});
        for (std::size_t lane = 0; lane < lanes.size(); ++lane)
        {
            fds[lane].fd = smb2_get_fd(lanes[lane]);
            fds[lane].events = static_cast<short>(smb2_which_events(lanes[lane]));
        }
        if (::poll(fds.data(), fds.size(), 250) < 0 && errno != EINTR)
        {
            rc = -errno;
            break;
        }
        for (std::size_t lane = 0; lane < lanes.size() && rc == 0; ++lane)
            if (fds[lane].revents != 0 && smb2_service(lanes[lane], fds[lane].revents) < 0)
            {
                walk.failed_lane = lane;
                rc = -EIO;
            }
        if (rc < 0)
            break;
        if (!walk.answered.empty())
            heard = std::time(nullptr);
        else if (std::time(nullptr) > heard + seconds_)
        {
            rc = -ETIMEDOUT;
            break;
        }
        for (WalkAsk *ask : walk.answered)
        {
            if (!stopped)
            {
                const std::string prefix = ask->path.empty() ? "" : ask->path + "/";
                for (const SmbEntry &entry : ask->entries)
                    if (entry.folder)
                        walk.waiting.push_back(prefix + entry.name);
                std::string inside = ask->path.substr(
                    std::min(ask->path.size(), location.path.size() + (location.path.empty() ? 0 : 1)));
                if (!inside.empty())
                    inside += "/";
                stopped = !found(inside, ask->entries);
            }
            delete ask;
        }
        walk.answered.clear();
    }
    if (rc == 0 && walk.failure != 0)
        rc = walk.failure;
    if (rc != 0)
    {
        failed(lanes[walk.failed_lane], rc, why);
        if (rc == -ETIMEDOUT)
            why->kind = SmbFailure::Kind::timeout;
        if (why->detail.empty() && !walk.failed_path.empty())
            why->detail = "Couldn't list " + walk.failed_path;
    }
    else
        *why = SmbFailure{};
    // Listings still in flight answer into `walk`: drop their sign-ins (the
    // answers then come back as shut down) rather than wait for them.
    for (std::size_t lane = 1; lane < lanes.size(); ++lane)
    {
        smb2_disconnect_share(lanes[lane]);
        smb2_destroy_context(lanes[lane]);
    }
    if (walk.in_flight[0] > 0 || rc != 0)
        detach();
    for (WalkAsk *ask : walk.answered)
        delete ask;
    return rc == 0 && !stopped;
}

} // namespace hui::concepts
