// NASBridge - NFS requests to the NAS.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/nfs.hpp"

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace hui::concepts
{

namespace
{

std::atomic<int> g_mount_port{0};

// "/a/b" from any mix of slashes ("/" for none).
std::string tidy_path(const std::string &text)
{
    std::string path = "/";
    for (const char c : text)
    {
        const char ch = c == '\\' ? '/' : c;
        if (ch == '/' && path.back() == '/')
            continue;
        path += ch;
    }
    if (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string lowered_ascii(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool contains(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

// libnfs reports an error as a negative errno, with its own words; a failed
// connection only shows up in the words ("connect() to server failed.
// Connection refused(111)").
NfsFailure::Kind kind_of(int error, const std::string &detail, bool mounting)
{
    if (contains(detail, "AUTH") || contains(detail, "auth error"))
        return NfsFailure::Kind::not_allowed; // e.g. an export that wants a privileged port
    if (contains(detail, "refused") || contains(detail, "PROG_UNAVAIL") ||
        contains(detail, "GETPORT"))
        return NfsFailure::Kind::refused;
    if (mounting && contains(detail, "POLLHUP"))
        return NfsFailure::Kind::refused; // how a refused connection shows on the first contact
    if (contains(detail, "timed out") || contains(detail, "imeout"))
        return NfsFailure::Kind::timeout;
    if (contains(detail, "unreachable") || contains(detail, "No route") ||
        contains(detail, "is down"))
        return NfsFailure::Kind::connect;
    switch (error)
    {
    case ENOENT:
    case ENOTDIR:
        return mounting ? NfsFailure::Kind::no_export : NfsFailure::Kind::not_found;
    case EACCES:
    case EPERM:
        return mounting ? NfsFailure::Kind::not_allowed : NfsFailure::Kind::denied;
    case ECONNREFUSED:
        return NfsFailure::Kind::refused;
    case ETIMEDOUT:
        return NfsFailure::Kind::timeout;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EHOSTDOWN:
    case ENETDOWN:
        return NfsFailure::Kind::connect;
    default:
        return NfsFailure::Kind::other;
    }
}

void fail(NfsFailure *why, int rc, const char *text, bool mounting)
{
    why->detail = text != nullptr ? text : "";
    while (!why->detail.empty() && (why->detail.back() == '\n' || why->detail.back() == ' '))
        why->detail.pop_back();
    why->kind = kind_of(rc < 0 ? -rc : rc, why->detail, mounting);
}

void failed(nfs_context *nfs, int rc, NfsFailure *why, bool mounting = false)
{
    fail(why, rc, nfs != nullptr ? nfs_get_error(nfs) : nullptr, mounting);
}

NfsEntry entry_of(const nfsdirent *item)
{
    NfsEntry entry;
    entry.name = item->name;
    entry.folder = item->type == NF3DIR;
    entry.size = entry.folder ? 0 : item->size;
    entry.modified = static_cast<std::uint64_t>(item->mtime.tv_sec);
    return entry;
}

// The export list, from the NAS's MOUNT service.
struct ExportsAsk
{
    bool finished = false;
    bool ok = false;
    std::string error;
    std::vector<std::string> dirs;
};

void exports_answer(rpc_context *rpc, int status, void *data, void *private_data)
{
    ExportsAsk *ask = static_cast<ExportsAsk *>(private_data);
    ask->finished = true;
    if (status != RPC_STATUS_SUCCESS || data == nullptr)
    {
        ask->error = rpc_get_error(rpc) != nullptr ? rpc_get_error(rpc) : "no answer";
        return;
    }
    for (exportnode *node = *static_cast<exports *>(data); node != nullptr; node = node->ex_next)
        if (node->ex_dir != nullptr)
            ask->dirs.push_back(node->ex_dir);
    ask->ok = true;
}

void exports_connected(rpc_context *rpc, int status, void *data, void *private_data)
{
    ExportsAsk *ask = static_cast<ExportsAsk *>(private_data);
    if (status != RPC_STATUS_SUCCESS)
    {
        ask->finished = true;
        ask->error = data != nullptr ? static_cast<const char *>(data)
                                     : (rpc_get_error(rpc) != nullptr ? rpc_get_error(rpc) : "");
        return;
    }
    if (rpc_mount3_export_task(rpc, exports_answer, ask) == nullptr)
    {
        ask->finished = true;
        ask->error = "Couldn't ask for the exports";
    }
}

} // namespace

NfsLocation nfs_location(const std::vector<std::string> &exports, const std::string &path)
{
    const std::string wanted = tidy_path(path);
    NfsLocation best;
    for (const std::string &raw : exports)
    {
        const std::string exported = tidy_path(raw);
        const bool inside =
            exported == "/" || wanted == exported ||
            (wanted.size() > exported.size() && wanted.compare(0, exported.size(), exported) == 0 &&
             wanted[exported.size()] == '/');
        if (!inside || (!best.exported.empty() && exported.size() <= best.exported.size()))
            continue;
        best.exported = exported;
        best.path = exported == "/" ? wanted : wanted.substr(exported.size());
        if (best.path.empty())
            best.path = "/";
    }
    return best;
}

bool nfs_skipped_name(const std::string &name)
{
    if (name.empty() || name[0] == '.')
        return true;
    const std::string lower = lowered_ascii(name);
    return lower == "@eadir" || lower == "#recycle" || lower == "$recycle.bin" ||
           lower == "system volume information" || lower == "#snapshot" ||
           lower == "lost+found";
}

std::string nfs_failure_brief(const NfsFailure &failure)
{
    switch (failure.kind)
    {
    case NfsFailure::Kind::none:
        return "no problem";
    case NfsFailure::Kind::address:
        return "not an IP address";
    case NfsFailure::Kind::connect:
        return "couldn't reach the NAS";
    case NfsFailure::Kind::refused:
        return "NFS isn't answering";
    case NfsFailure::Kind::timeout:
        return "the NAS stopped answering";
    case NfsFailure::Kind::no_export:
        return "not in an NFS export";
    case NfsFailure::Kind::not_allowed:
        return "the NAS refused this PS5";
    case NfsFailure::Kind::not_found:
        return "not found";
    case NfsFailure::Kind::denied:
        return "access denied";
    case NfsFailure::Kind::other:
        break;
    }
    return failure.detail.empty() ? std::string("NFS error") : failure.detail;
}

void describe_nfs_failure(const Connection &connection, const NfsFailure &failure,
                          std::string *headline, std::string *detail)
{
    const std::string where = connection.host + ":" + std::to_string(connection.effective_port());
    switch (failure.kind)
    {
    case NfsFailure::Kind::none:
        *headline = "Connected";
        detail->clear();
        return;
    case NfsFailure::Kind::address:
        *headline = "Enter the NAS's IP address";
        *detail = "NASBridge can't look up names yet: use the NAS's IPv4 address, "
                  "such as 192.168.1.20.";
        return;
    case NfsFailure::Kind::connect:
        *headline = "Can't reach the NAS";
        *detail = "Nothing answered at " + connection.host +
                  ". Check the address, and that the NAS and the PS5 are on the same network.";
        return;
    case NfsFailure::Kind::refused:
        *headline = "NFS isn't answering";
        *detail = "The NAS is there, but its NFS service isn't answering (ports 111 and " +
                  std::to_string(connection.effective_port()) +
                  "). Turn on NFS in the NAS's settings.";
        return;
    case NfsFailure::Kind::timeout:
        *headline = "The NAS stopped answering";
        *detail = "No reply from " + where + " in time. Check the NAS is awake and try again.";
        return;
    case NfsFailure::Kind::no_export:
        *headline = "Not in an NFS export";
        *detail = "None of the NAS's NFS exports holds " + connection.folder +
                  ". Export the share over NFS (Unraid: the share's NFS Security Settings, "
                  "Export: Yes), then pick the folder with the folder button.";
        return;
    case NfsFailure::Kind::not_allowed:
        *headline = "The NAS won't let this PS5 in";
        *detail = "The export isn't open to this PS5. Allow its IP address in the export's "
                  "settings (Unraid: Security Public, or a rule for the PS5).";
        if (!nfs_privileged_ports())
            *detail += " NASBridge can't use a privileged port, so the export also needs the "
                       "insecure option (Synology: allow connections from non-privileged ports).";
        return;
    case NfsFailure::Kind::not_found:
        *headline = "Folder not found";
        *detail = "The NAS has no folder " + connection.folder +
                  ". Pick the games folder with the folder button.";
        return;
    case NfsFailure::Kind::denied:
        *headline = "Not allowed to read this folder";
        *detail = "The export is open to this PS5, but NASBridge reads as user " +
                  std::to_string(kNfsUid) + ", group " + std::to_string(kNfsGid) +
                  " (nobody, users), who can't read " + connection.folder +
                  ". Make the games readable by everyone.";
        return;
    case NfsFailure::Kind::other:
        break;
    }
    *headline = "Couldn't read the NAS";
    *detail = failure.detail.empty() ? std::string("Something went wrong talking to ") + where + "."
                                     : failure.detail;
}

bool nfs_privileged_ports()
{
    static std::mutex lock;
    static int known = -1;
    std::lock_guard<std::mutex> hold(lock);
    if (known >= 0)
        return known == 1;
    known = 0;
    for (int port = 1023; port >= 600; --port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            break;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        const int rc = ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        const int error = errno;
        ::close(fd);
        if (rc == 0)
        {
            known = 1;
            break;
        }
        if (error != EADDRINUSE)
            break; // EACCES: never allowed
    }
    return known == 1;
}

void set_nfs_mount_port(int port)
{
    g_mount_port = port;
}

int nfs_mount_port()
{
    return g_mount_port;
}

NfsSession::NfsSession(const Connection &connection, int seconds)
    : connection_(connection), seconds_(seconds)
{
}

NfsSession::~NfsSession()
{
    detach();
}

void NfsSession::detach()
{
    if (nfs_ == nullptr)
        return;
    nfs_destroy_context(nfs_);
    nfs_ = nullptr;
    exported_.clear();
}

bool NfsSession::list_exports(std::vector<std::string> *exports, NfsFailure *why)
{
    *why = NfsFailure{};
    if (have_exports_)
    {
        *exports = exports_;
        return true;
    }
    exports->clear();
    in_addr address{};
    if (::inet_pton(AF_INET, connection_.host.c_str(), &address) != 1)
    {
        why->kind = NfsFailure::Kind::address;
        return false;
    }
    rpc_context *rpc = rpc_init_context();
    if (rpc == nullptr)
    {
        why->kind = NfsFailure::Kind::other;
        why->detail = "Out of memory";
        return false;
    }
    ExportsAsk ask;
    const int port = g_mount_port;
    int rc = port > 0 ? rpc_connect_port_async(rpc, connection_.host.c_str(), port, MOUNT_PROGRAM,
                                               MOUNT_V3, exports_connected, &ask)
                      : rpc_connect_program_async(rpc, connection_.host.c_str(), MOUNT_PROGRAM,
                                                  MOUNT_V3, exports_connected, &ask);
    if (rc != 0)
        ask.error = rpc_get_error(rpc) != nullptr ? rpc_get_error(rpc) : "Couldn't connect";
    const std::time_t deadline = std::time(nullptr) + seconds_ + 1;
    bool timed_out = false;
    while (rc == 0 && !ask.finished)
    {
        pollfd pfd{};
        pfd.fd = rpc_get_fd(rpc);
        pfd.events = static_cast<short>(rpc_which_events(rpc));
        if (::poll(&pfd, 1, 250) < 0 && errno != EINTR)
            break;
        if (rpc_service(rpc, pfd.revents) < 0)
        {
            if (!ask.finished)
                ask.error = rpc_get_error(rpc) != nullptr ? rpc_get_error(rpc) : "Lost the NAS";
            break;
        }
        if (std::time(nullptr) > deadline)
        {
            timed_out = true;
            break;
        }
    }
    rpc_destroy_context(rpc); // answers still out are called back here, as cancelled
    if (!ask.ok)
    {
        fail(why, timed_out ? -ETIMEDOUT : 0, ask.error.c_str(), true);
        if (timed_out)
            why->kind = NfsFailure::Kind::timeout;
        return false;
    }
    std::sort(ask.dirs.begin(), ask.dirs.end(),
              [](const std::string &a, const std::string &b)
              { return lowered_ascii(a) < lowered_ascii(b); });
    exports_ = ask.dirs;
    have_exports_ = true;
    *exports = exports_;
    return true;
}

bool NfsSession::locate(const std::string &path, NfsLocation *location, NfsFailure *why)
{
    std::vector<std::string> exports;
    if (!list_exports(&exports, why))
        return false;
    *location = nfs_location(exports, path);
    if (location->exported.empty())
    {
        why->kind = NfsFailure::Kind::no_export;
        return false;
    }
    return true;
}

nfs_context *NfsSession::connect(const std::string &exported, NfsFailure *why)
{
    *why = NfsFailure{};
    nfs_context *nfs = nfs_init_context();
    if (nfs == nullptr)
    {
        why->kind = NfsFailure::Kind::other;
        why->detail = "Out of memory";
        return nullptr;
    }
    nfs_set_timeout(nfs, seconds_ * 1000);
    nfs_set_uid(nfs, kNfsUid);
    nfs_set_gid(nfs, kNfsGid);
    nfs_set_version(nfs, NFS_V3);
    nfs_set_autoreconnect(nfs, 0); // a lost connection fails the call: the caller tries again
    nfs_set_dircache(nfs, 0);      // listings as they are now (the library refreshes)
    nfs_set_auto_traverse_mounts(nfs, 0);
    nfs_set_nfsport(nfs, connection_.effective_port());
    if (g_mount_port > 0)
        nfs_set_mountport(nfs, g_mount_port);
    const int rc = nfs_mount(nfs, connection_.host.c_str(), exported.c_str());
    if (rc < 0)
    {
        failed(nfs, rc, why, true);
        nfs_destroy_context(nfs);
        return nullptr;
    }
    return nfs;
}

bool NfsSession::attach(const std::string &exported, NfsFailure *why)
{
    *why = NfsFailure{};
    if (nfs_ != nullptr && exported_ == exported)
        return true;
    detach();
    nfs_ = connect(exported, why);
    if (nfs_ == nullptr)
        return false;
    exported_ = exported;
    return true;
}

bool NfsSession::list(const std::string &folder, std::vector<NfsEntry> *entries, NfsFailure *why)
{
    entries->clear();
    std::vector<std::string> exports;
    if (!list_exports(&exports, why))
        return false;
    const NfsLocation location = nfs_location(exports, folder);
    if (location.exported.empty())
    {
        // Above the exports: the next folder on the way to each.
        const std::string here = tidy_path(folder);
        const std::string prefix = here == "/" ? "/" : here + "/";
        std::vector<std::string> names;
        for (const std::string &raw : exports)
        {
            const std::string exported = tidy_path(raw);
            if (exported.size() <= prefix.size() || exported.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string rest = exported.substr(prefix.size());
            const std::string name = rest.substr(0, rest.find('/'));
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
        if (names.empty())
        {
            why->kind = NfsFailure::Kind::no_export;
            return false;
        }
        for (std::string &name : names)
        {
            NfsEntry entry;
            entry.name = std::move(name);
            entry.folder = true;
            entries->push_back(std::move(entry));
        }
        return true;
    }
    if (!attach(location.exported, why))
        return false;
    nfsdir *dir = nullptr;
    const int rc = nfs_opendir(nfs_, location.path.c_str(), &dir);
    if (rc < 0)
    {
        failed(nfs_, rc, why);
        if (why->kind != NfsFailure::Kind::not_found && why->kind != NfsFailure::Kind::denied)
            detach();
        return false;
    }
    while (const nfsdirent *item = nfs_readdir(nfs_, dir))
    {
        if (item->name == nullptr || nfs_skipped_name(item->name))
            continue;
        entries->push_back(entry_of(item));
    }
    nfs_closedir(nfs_, dir);
    return true;
}

bool NfsSession::read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                            std::string *out, NfsFailure *why)
{
    out->clear();
    NfsLocation location;
    if (!locate(file, &location, why) || !attach(location.exported, why))
        return false;
    nfsfh *handle = nullptr;
    int rc = nfs_open(nfs_, location.path.c_str(), O_RDONLY, &handle);
    if (rc < 0)
    {
        failed(nfs_, rc, why);
        return false;
    }
    out->resize(length);
    const std::size_t most = std::max<std::size_t>(nfs_get_readmax(nfs_), 64u * 1024u);
    std::size_t got = 0;
    bool ok = true;
    while (got < length)
    {
        const std::size_t ask = std::min(length - got, most);
        rc = nfs_pread(nfs_, handle, &(*out)[got], ask, offset + got);
        if (rc <= 0) // 0: the file ended before `length` bytes
        {
            if (rc < 0)
                failed(nfs_, rc, why);
            else
            {
                why->kind = NfsFailure::Kind::other;
                why->detail = "The file is shorter than expected";
            }
            ok = false;
            break;
        }
        got += static_cast<std::size_t>(rc);
    }
    nfs_close(nfs_, handle);
    if (!ok)
    {
        out->clear();
        if (why->kind != NfsFailure::Kind::other || rc < 0)
            detach();
    }
    return ok;
}

bool NfsSession::read_file(const std::string &file, std::size_t max, std::string *out,
                           bool *too_big, NfsFailure *why)
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

bool NfsSession::stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                        const std::function<bool(const char *data, std::size_t length)> &sink,
                        NfsFailure *why)
{
    NfsLocation location;
    if (!locate(file, &location, why) || !attach(location.exported, why))
        return false;
    nfsfh *handle = nullptr;
    int rc = nfs_open(nfs_, location.path.c_str(), O_RDONLY, &handle);
    if (rc < 0)
    {
        failed(nfs_, rc, why);
        return false;
    }
    nfs_stat_64 info{};
    rc = nfs_fstat64(nfs_, handle, &info);
    if (rc < 0)
    {
        failed(nfs_, rc, why);
        nfs_close(nfs_, handle);
        return false;
    }
    if (size != nullptr)
        *size = info.nfs_size;
    // The biggest read the NAS takes, up to 1 MiB (a read is one round trip).
    const std::size_t most =
        std::min<std::size_t>(std::max<std::size_t>(nfs_get_readmax(nfs_), 64u * 1024u), 1u << 20);
    std::vector<char> buffer(most);
    std::uint64_t at = offset;
    bool ok = true;
    while (at < info.nfs_size)
    {
        const std::size_t ask = static_cast<std::size_t>(std::min<std::uint64_t>(info.nfs_size - at, most));
        rc = nfs_pread(nfs_, handle, buffer.data(), ask, at);
        if (rc <= 0)
        {
            if (rc < 0)
                failed(nfs_, rc, why);
            else
            {
                why->kind = NfsFailure::Kind::other;
                why->detail = "The file is shorter than expected";
            }
            ok = false;
            break;
        }
        at += static_cast<std::uint64_t>(rc);
        if (!sink(buffer.data(), static_cast<std::size_t>(rc)))
        {
            ok = false;
            break;
        }
    }
    nfs_close(nfs_, handle);
    if (!ok && why->kind != NfsFailure::Kind::none)
        detach(); // a failed read may leave the connection mid-reply
    return ok;
}

namespace
{

struct NfsWalkAsk;

// One walk's shared state: the folders still to list, and the answers.
struct NfsWalk
{
    std::vector<int> in_flight; // per connection
    int failure = 0;            // the first failed listing's status
    std::size_t failed_lane = 0;
    std::string failed_path;
    std::deque<std::string> waiting;    // paths in the export
    std::vector<NfsWalkAsk *> answered; // listed, not yet handed to `found`
};

struct NfsWalkAsk
{
    NfsWalk *walk;
    std::size_t lane; // which connection asked
    std::string path;
    std::vector<NfsEntry> entries;
};

void walk_answer(int status, nfs_context *nfs, void *data, void *private_data)
{
    NfsWalkAsk *ask = static_cast<NfsWalkAsk *>(private_data);
    NfsWalk *walk = ask->walk;
    --walk->in_flight[ask->lane];
    if (status != 0 || data == nullptr)
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
    nfsdir *dir = static_cast<nfsdir *>(data);
    while (const nfsdirent *item = nfs_readdir(nfs, dir))
    {
        if (item->name == nullptr || nfs_skipped_name(item->name))
            continue;
        ask->entries.push_back(entry_of(item));
    }
    nfs_closedir(nfs, dir);
    walk->answered.push_back(ask);
}

} // namespace

bool NfsSession::walk(
    const std::string &folder, int lanes, int at_once,
    const std::function<bool(const std::string &inside, const std::vector<NfsEntry> &entries)> &found,
    NfsFailure *why)
{
    NfsLocation location;
    if (!locate(folder, &location, why) || !attach(location.exported, why))
        return false;
    lanes = std::max(lanes, 1);
    at_once = std::max(at_once, 1);
    // Lane 0 is this session's own connection; the others are made for the
    // walk and closed after it. A NAS slow to read its disks answers one
    // connection's requests one after another, so more connections help; a
    // few requests in flight on each hide the network's delay as well.
    std::vector<nfs_context *> contexts{nfs_};
    NfsWalk walk;
    walk.in_flight.push_back(0);
    walk.waiting.push_back(location.path);
    const std::string top = location.path == "/" ? "" : location.path;
    bool stopped = false;
    int rc = 0;
    bool timed_out = false;
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
        // More connections once there is more than one folder to list (a
        // small folder never needs them). One that fails just isn't used.
        if (static_cast<int>(contexts.size()) < lanes && walk.waiting.size() > 1)
        {
            NfsFailure ignored;
            nfs_context *extra = connect(location.exported, &ignored);
            if (extra == nullptr)
                lanes = static_cast<int>(contexts.size());
            else
            {
                contexts.push_back(extra);
                walk.in_flight.push_back(0);
            }
        }
        for (std::size_t lane = 0; lane < contexts.size() && rc == 0; ++lane)
            while (walk.in_flight[lane] < at_once && !walk.waiting.empty())
            {
                NfsWalkAsk *ask = new NfsWalkAsk{&walk, lane, std::move(walk.waiting.front()), {}};
                walk.waiting.pop_front();
                rc = nfs_opendir_async(contexts[lane], ask->path.c_str(), walk_answer, ask);
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
        fds.assign(contexts.size(), pollfd{});
        for (std::size_t lane = 0; lane < contexts.size(); ++lane)
        {
            fds[lane].fd = nfs_get_fd(contexts[lane]);
            fds[lane].events = static_cast<short>(nfs_which_events(contexts[lane]));
        }
        if (::poll(fds.data(), fds.size(), 250) < 0 && errno != EINTR)
        {
            rc = -errno;
            break;
        }
        for (std::size_t lane = 0; lane < contexts.size() && rc == 0; ++lane)
            if (nfs_service(contexts[lane], fds[lane].revents) < 0)
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
            timed_out = true;
            break;
        }
        for (NfsWalkAsk *ask : walk.answered)
        {
            if (!stopped)
            {
                const std::string prefix = ask->path == "/" ? "/" : ask->path + "/";
                for (const NfsEntry &entry : ask->entries)
                    if (entry.folder)
                        walk.waiting.push_back(prefix + entry.name);
                std::string inside = ask->path.substr(std::min(ask->path.size(), top.size()));
                if (!inside.empty() && inside[0] == '/')
                    inside.erase(0, 1);
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
        failed(contexts[walk.failed_lane], rc, why);
        if (timed_out)
            why->kind = NfsFailure::Kind::timeout;
        if (why->detail.empty() && !walk.failed_path.empty())
            why->detail = "Couldn't list " + walk.failed_path;
    }
    else
        *why = NfsFailure{};
    // Listings still in flight answer into `walk` as their contexts go:
    // close the extra connections rather than wait for them.
    for (std::size_t lane = 1; lane < contexts.size(); ++lane)
        nfs_destroy_context(contexts[lane]);
    if (walk.in_flight[0] > 0 || rc != 0)
        detach();
    for (NfsWalkAsk *ask : walk.answered)
        delete ask;
    return rc == 0 && !stopped;
}

} // namespace hui::concepts
