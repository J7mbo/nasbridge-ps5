// NASBridge - How much room the PS5's own storage has.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Several ways are tried, first answer that can be the console's storage
// wins. Nothing is ever called blindly: a function the console doesn't have
// is left at address 0 by the loader, and calling that crashes the app (seen
// on 13.60). So each one is either declared weak and checked, or looked up by
// name at run time with dlsym, which answers "not there" instead.
//
//   1. ShellCoreUtil's free size of the user partition (what Settings shows).
//   2. libkernel's statfs, looked up by name, on /user, /data and /app0.
//   3. libkernel's getfsstat, looked up by name: the mount on /user.
//   4. libkernel's _fstatfs on those folders opened.
// (On 13.60 none of these answer in NASBridge's sandbox; ShadowMountPlus does,
// see storage.hpp.)
//
// No helper process: ShadowMountPlus is already running, and a request to it
// is an ordinary local HTTP call.

#include "concepts/storage.hpp"

#include "concepts/connection.hpp"
#include "concepts/file_calls.hpp"
#include "concepts/http.hpp"
#include "concepts/json_scan.hpp"
#include "concepts/smp.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__PROSPERO__)
#include <sys/cpuset.h>
#include <pthread_np.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <vector>

extern "C" {
// libSceSystemService. Free and total bytes of the user partition.
__attribute__((weak)) int sceShellCoreUtilGetFreeSizeOfUserPartition(std::uint64_t *free_size,
                                                                     std::uint64_t *total_size);
// libkernel's statfs on an open file.
__attribute__((weak)) int _fstatfs(int fd, struct statfs *buffer);
// libkernel's run-time lookup (declared here rather than via <dlfcn.h>, so
// they can be weak too).
__attribute__((weak)) void *dlopen(const char *path, int mode);
__attribute__((weak)) void *dlsym(void *handle, const char *name);
int sceKernelGetCurrentCpu(void);
}
#endif

namespace hui::concepts
{

#if defined(__PROSPERO__)
namespace
{

constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
constexpr const char *kFolders[] = {"/user", "/data", "/app0"};

using StatfsFn = int (*)(const char *, struct statfs *);
using GetfsstatFn = int (*)(struct statfs *, long, int);

// The address of an imported function as the loader filled it in; volatile
// so the compiler can't assume a declared function is always there.
template <typename F> F *imported(F *function)
{
    F *volatile address = function;
    return address;
}

// An answer that can't be the console's storage is ignored.
bool plausible(std::uint64_t free, std::uint64_t total)
{
    return total >= 16 * kGiB && free <= total;
}

double gib(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / 1e9;
}

// What the lookups found, for the reason shown on screen: -1 no dlopen or
// dlsym at all, otherwise how many libkernel handles dlopen gave.
int g_handles = -1;

// Looks `name` up in libkernel by name, once per process. Null when the
// console's libkernel doesn't have it.
void *kernel_function(const char *name)
{
    auto *open_library = imported(&dlopen);
    auto *find = imported(&dlsym);
    if (open_library == nullptr || find == nullptr)
        return nullptr;
    g_handles = 0;
    // Already loaded in every app; dlopen only hands back their handles.
    for (const char *library : {"libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx"})
    {
        void *handle = open_library(library, 0x1 /* RTLD_LAZY */);
        if (handle == nullptr)
            continue;
        ++g_handles;
        if (void *function = find(handle, name))
            return function;
    }
    return nullptr;
}

struct Functions
{
    bool looked_up = false;
    StatfsFn statfs = nullptr;
    GetfsstatFn getfsstat = nullptr;
};

Functions &functions()
{
    static Functions found;
    if (!found.looked_up)
    {
        found.looked_up = true;
        found.statfs = reinterpret_cast<StatfsFn>(kernel_function("statfs"));
        found.getfsstat = reinterpret_cast<GetfsstatFn>(kernel_function("getfsstat"));
    }
    return found;
}

// Takes `info` when it can be the console's storage; otherwise adds why not
// to *why.
bool take(const struct statfs &info, const char *where, StorageSpace *space, std::string *why)
{
    if (info.f_bsize == 0 || info.f_bavail < 0)
    {
        *why += std::string(" ") + where + ":bad";
        return false;
    }
    const std::uint64_t room = static_cast<std::uint64_t>(info.f_bavail) * info.f_bsize;
    const std::uint64_t size = info.f_blocks * info.f_bsize;
    if (!plausible(room, size))
    {
        char text[64];
        std::snprintf(text, sizeof(text), " %s:%.1f/%.1fGB", where, gib(room), gib(size));
        *why += text;
        return false;
    }
    space->known = true;
    space->free = room;
    space->total = size;
    return true;
}

void add_errno(std::string *why, const char *where)
{
    char text[48];
    std::snprintf(text, sizeof(text), " %s:e%d", where, errno);
    *why += text;
}

} // namespace
#endif

StorageSpace internal_storage()
{
    StorageSpace space;
#if defined(__PROSPERO__)
    std::string why;

    // 1. ShellCoreUtil. Room to spare behind each answer, in case the call
    // writes more than one number per argument on this firmware.
    if (auto *free_size = imported(&sceShellCoreUtilGetFreeSizeOfUserPartition))
    {
        std::uint64_t free_out[4] = {};
        std::uint64_t total_out[4] = {};
        const int result = free_size(free_out, total_out);
        if (result == 0 && plausible(free_out[0], total_out[0]))
        {
            space.known = true;
            space.free = free_out[0];
            space.total = total_out[0];
            space.how = "shellcore";
            return space;
        }
        char text[64];
        std::snprintf(text, sizeof(text), "shellcore:%x", static_cast<unsigned>(result));
        why += text;
    }
    else
    {
        why += "shellcore:none";
    }

    // 2. statfs by folder name.
    Functions &found = functions();
    if (found.statfs != nullptr)
    {
        why += " | statfs";
        for (const char *folder : kFolders)
        {
            struct statfs info;
            std::memset(&info, 0, sizeof(info));
            if (found.statfs(folder, &info) != 0)
            {
                add_errno(&why, folder);
                continue;
            }
            if (take(info, folder, &space, &why))
            {
                space.how = std::string("statfs ") + folder;
                return space;
            }
        }
    }
    else
    {
        char text[48];
        std::snprintf(text, sizeof(text), " | statfs:none (libs %d)", g_handles);
        why += text;
    }

    // 3. getfsstat: every mount, the one on /user.
    if (found.getfsstat != nullptr)
    {
        why += " | mounts";
        const int count = found.getfsstat(nullptr, 0, 2 /* MNT_NOWAIT */);
        if (count > 0 && count < 512)
        {
            std::vector<struct statfs> mounts(static_cast<std::size_t>(count) + 8);
            const int got = found.getfsstat(
                mounts.data(), static_cast<long>(mounts.size() * sizeof(struct statfs)), 2);
            for (int i = 0; i < got && i < static_cast<int>(mounts.size()); ++i)
            {
                mounts[static_cast<std::size_t>(i)].f_mntonname[MNAMELEN - 1] = '\0';
                if (std::strcmp(mounts[static_cast<std::size_t>(i)].f_mntonname, "/user") == 0 &&
                    take(mounts[static_cast<std::size_t>(i)], "/user", &space, &why))
                {
                    space.how = "mounts /user";
                    return space;
                }
            }
            char text[32];
            std::snprintf(text, sizeof(text), ":%d/%d", got, count);
            why += text;
        }
        else
        {
            add_errno(&why, "count");
        }
    }
    else
    {
        why += " | mounts:none";
    }

    // 4. _fstatfs on the folders opened.
    if (auto *fstatfs_call = imported(&_fstatfs))
    {
        why += " | fstatfs";
        for (const char *folder : kFolders)
        {
            const int fd = open(folder, O_RDONLY);
            if (fd < 0)
            {
                add_errno(&why, folder);
                continue;
            }
            struct statfs info;
            std::memset(&info, 0, sizeof(info));
            const int result = fstatfs_call(fd, &info);
            const int error = errno;
            close(fd);
            if (result != 0)
            {
                errno = error;
                add_errno(&why, folder);
                continue;
            }
            if (take(info, folder, &space, &why))
            {
                space.how = std::string("fstatfs ") + folder;
                return space;
            }
        }
    }
    else
    {
        why += " | fstatfs:none";
    }
    space.how = why;
#else
    // The PC preview: a fixed value for the snapshots.
    space.known = true;
    space.free = 612ull * 1000ull * 1000ull * 1000ull;
    space.total = 1000ull * 1000ull * 1000ull * 1000ull;
    space.how = "preview";
    Destination internal;
    internal.path = "/data/homebrew";
    internal.mount_point = "/user";
    internal.free = space.free;
    internal.total = space.total;
    internal.writable = 1;
    Destination usb;
    usb.path = "/mnt/usb0/homebrew";
    usb.mount_point = "/mnt/usb0";
    usb.free = 1800ull * 1000ull * 1000ull * 1000ull;
    usb.total = 2000ull * 1000ull * 1000ull * 1000ull;
    usb.writable = 1;
    space.destinations = {internal, usb};
    space.drives_known = true;
    space.drives = {"/mnt/usb0"};
#endif
    return space;
}

// ---- ShadowMountPlus ----

namespace
{

struct Mount
{
    std::string point;
    std::string path; // destinations only
    std::uint64_t total = 0;
    std::uint64_t available = 0;
    bool read_only = false;
};

// The objects of the array `key` ("mounts" or "destinations").
std::vector<Mount> json_mounts(const std::string &json, const char *key = "mounts")
{
    std::vector<Mount> mounts;
    json_objects(json, key,
                 [&](const char *from, const char *to)
                 {
                     Mount mount;
                     mount.point = json_text(from, to, "mount_point");
                     mount.path = json_text(from, to, "path");
                     mount.read_only = json_true(from, to, "read_only");
                     if (json_number(from, to, "total_bytes", &mount.total) &&
                         json_number(from, to, "available_bytes", &mount.available))
                         mounts.push_back(mount);
                 });
    return mounts;
}

std::uint64_t now_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

} // namespace

StorageSpace storage_from_json(const std::string &json)
{
    StorageSpace space;
    space.asked = true;
    for (const Mount &found : json_mounts(json, "destinations"))
    {
        if (found.path.empty() || found.path[0] != '/')
            continue;
        Destination destination;
        destination.path = found.path;
        destination.mount_point = found.point;
        destination.free = found.available;
        destination.total = found.total;
        destination.read_only = found.read_only;
        space.destinations.push_back(destination);
    }
    const std::vector<Mount> mounts = json_mounts(json);
    // The drives: what is mounted on /mnt/usbN and /mnt/extN, and the
    // drives ShadowMountPlus lists folders on.
    space.drives_known = true;
    for (const Mount &mount : mounts)
    {
        if (mount.point.empty() || drive_of(mount.point) != mount.point)
            continue;
        if (std::find(space.drives.begin(), space.drives.end(), mount.point) != space.drives.end())
            continue;
        space.drives.push_back(mount.point);
        space.drive_space.push_back({mount.point, mount.available, mount.total, mount.read_only});
    }
    for (const Destination &destination : space.destinations)
    {
        const std::string drive = drive_of(destination.path);
        if (!drive.empty() &&
            std::find(space.drives.begin(), space.drives.end(), drive) == space.drives.end())
            space.drives.push_back(drive);
    }
    // The console's own storage, by the names it is mounted under.
    for (const char *wanted : {"/user", "/data", "/system_data/priv", "/"})
    {
        for (const Mount &mount : mounts)
        {
            if (mount.point == wanted && mount.total >= 16ull * 1024 * 1024 * 1024 &&
                mount.available <= mount.total)
            {
                space.known = true;
                space.free = mount.available;
                space.total = mount.total;
                space.how = "shadowmount " + mount.point;
                return space;
            }
        }
    }
    // Not found: list what was there, so the right one can be added.
    space.how = "shadowmount: no internal mount among";
    if (mounts.empty())
        space.how += " (none)";
    for (const Mount &mount : mounts)
    {
        char text[160];
        std::snprintf(text, sizeof(text), " %s=%.0f/%.0fGB", mount.point.c_str(),
                      static_cast<double>(mount.available) / 1e9,
                      static_cast<double>(mount.total) / 1e9);
        space.how += text;
    }
    return space;
}

StorageSpace shadowmount_storage()
{
    Connection local;
    local.host = "127.0.0.1";
    local.port = smp_port();
    const HttpReply reply = http_request(local, "POST", "/api/v1/storage",
                                         "Content-Type: application/json\r\n", "{}",
                                         256u * 1024u);
    if (!reply.connected)
    {
        StorageSpace space;
        space.asked = true;
        char text[96];
        std::snprintf(text, sizeof(text), "shadowmount: not answering (e%d)", reply.error);
        space.how = text;
        return space;
    }
    if (reply.status != 200)
    {
        StorageSpace space;
        space.asked = true;
        char text[96];
        std::snprintf(text, sizeof(text), "shadowmount: status %d", reply.status);
        space.how = text;
        return space;
    }
    return storage_from_json(reply.body);
}

std::string drive_of(const std::string &path)
{
    static const char *const kDrives[] = {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                          "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
                                          "/mnt/ext0", "/mnt/ext1"};
    for (const char *drive : kDrives)
    {
        const std::size_t length = std::strlen(drive);
        if (path.compare(0, length, drive) == 0 &&
            (path.size() == length || path[length] == '/'))
            return drive;
    }
    return "";
}

bool drive_connected(const StorageSpace &space, const std::string &path)
{
    const std::string drive = drive_of(path);
    if (drive.empty() || !space.drives_known)
        return true;
    return std::find(space.drives.begin(), space.drives.end(), drive) != space.drives.end();
}

void merge_destinations(StorageSpace *space)
{
    constexpr std::uint64_t kSmallestDrive = 2ull * 1000 * 1000 * 1000; // bytes
    std::vector<Destination> merged;
    // Internal storage: ShadowMountPlus's numbers for it, or the console's.
    Destination internal;
    internal.path = kInternalGames;
    internal.mount_point = "/data";
    internal.free = space->known ? space->free : 0;
    internal.total = space->known ? space->total : 0;
    for (const Destination &listed : space->destinations)
        if (listed.path == kInternalGames)
            internal = listed;
    merged.push_back(internal);
    // One folder per drive: <drive>/homebrew, a folder ShadowMountPlus scans
    // (made by the first copy when it isn't there yet).
    std::vector<std::string> drives = space->drives;
    if (!space->drives_known)
    {
        // Not known this time: the drives ShadowMountPlus listed folders on.
        for (const Destination &listed : space->destinations)
        {
            const std::string drive = drive_of(listed.path);
            if (!drive.empty() && std::find(drives.begin(), drives.end(), drive) == drives.end())
                drives.push_back(drive);
        }
    }
    std::sort(drives.begin(), drives.end(),
              [](const std::string &a, const std::string &b)
              {
                  // USB drives first, then extended storage, each by number.
                  const bool a_usb = a.compare(0, 8, "/mnt/usb") == 0;
                  const bool b_usb = b.compare(0, 8, "/mnt/usb") == 0;
                  return a_usb != b_usb ? a_usb : a < b;
              });
    for (const std::string &drive : drives)
    {
        Destination place;
        place.path = drive + "/homebrew";
        place.mount_point = drive;
        for (const DriveSpace &mount : space->drive_space)
            if (mount.mount_point == drive)
            {
                place.free = mount.free;
                place.total = mount.total;
                place.read_only = mount.read_only;
            }
        // ShadowMountPlus's own listing of the drive (or its homebrew folder)
        // wins over the mount's numbers.
        for (const Destination &listed : space->destinations)
            if (listed.path == drive || listed.path == place.path)
            {
                place.free = listed.free;
                place.total = listed.total;
                place.read_only = listed.read_only;
                if (!listed.mount_point.empty())
                    place.mount_point = listed.mount_point;
            }
        // A partition too small for any game isn't a choice: the ~200 MB EFI
        // partition a Mac (or Windows, on GPT) puts on a drive, which the
        // PS5 mounts as a drive of its own.
        if (place.total > 0 && place.total < kSmallestDrive)
            continue;
        merged.push_back(place);
    }
    space->destinations = std::move(merged);
}

bool probe_drives(std::vector<std::string> *drives)
{
    drives->clear();
#if defined(__PROSPERO__)
    // An empty /mnt/usbN belongs to /mnt; a mounted drive is another device
    // (how ShadowMountPlus tells too).
    struct stat parent;
    if (::stat("/mnt", &parent) != 0)
        return false;
    bool any = false;
    for (const char *drive : {"/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
                              "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1"})
    {
        struct stat info;
        if (::stat(drive, &info) != 0)
            continue;
        any = true;
        if (info.st_dev != parent.st_dev)
            drives->push_back(drive);
    }
    return any;
#else
    return false; // the PC preview: its fixed list
#endif
}

std::string destination_name(const std::string &path)
{
    if (path == "/data/homebrew" || path.rfind("/data/", 0) == 0 || path == "/data")
        return "Internal storage";
    // "/mnt/ext0/..." and "/mnt/usb3/...": the drive's kind and number.
    const auto numbered = [&](const char *prefix, const char *name, bool first_plain)
    {
        const std::size_t length = std::strlen(prefix);
        if (path.compare(0, length, prefix) != 0 || path.size() <= length ||
            path[length] < '0' || path[length] > '9')
            return std::string();
        const int number = path[length] - '0';
        if (first_plain && number == 0)
            return std::string(name);
        return std::string(name) + " " + std::to_string(number + 1);
    };
    std::string name = numbered("/mnt/ext", "Extended storage", true);
    if (name.empty())
        name = numbered("/mnt/usb", "USB drive", false);
    return name.empty() ? path : name;
}

void check_destination(Destination *destination)
{
    destination->writable = 0;
    if (destination->read_only)
    {
        destination->why = "read-only";
        return;
    }
    std::string missing;
    if (!file_calls_available(&missing))
    {
        destination->why = "no " + missing;
        return;
    }
    const auto fail = [&](const char *step)
    {
        char text[48];
        std::snprintf(text, sizeof(text), "%s e%d", step, errno);
        destination->why = text;
    };
    // Laid out like a copy: a folder of NASBridge's, a file written in
    // pieces, reopened to carry on, renamed when done, then removed.
    // A folder not made yet (a drive's homebrew before its first copy):
    // where the copy will make it.
    std::string where = destination->path;
    struct stat info;
    if (::stat(where.c_str(), &info) != 0 && errno == ENOENT)
    {
        const std::size_t slash = where.rfind('/');
        if (slash != std::string::npos && slash > 0)
            where.resize(slash);
    }
    const std::string folder = where + "/.nasbridge-check";
    const std::string part = folder + "/check.part";
    const std::string done = folder + "/check.bin";
    if (::mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST)
        return fail("folder");
    int fd = ::open(part.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd < 0)
    {
        fail("create");
        ::rmdir(folder.c_str());
        return;
    }
    std::vector<unsigned char> block(256 * 1024);
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<unsigned char>(i * 31u + 7u);
    bool ok = true;
    const char *step = nullptr;
    for (int i = 0; i < 4 && ok; ++i)
        if (::write(fd, block.data(), block.size()) != static_cast<ssize_t>(block.size()))
            ok = false, step = "write";
    if (ok && ::fsync(fd) != 0)
        ok = false, step = "sync";
    ::close(fd);
    // Reopened as a resumed copy would: its size says where to carry on.
    if (ok)
    {
        fd = ::open(part.c_str(), O_RDWR);
        if (fd < 0)
            ok = false, step = "reopen";
        else
        {
            const off_t size = ::lseek(fd, 0, SEEK_END);
            std::vector<unsigned char> back(block.size());
            if (size != static_cast<off_t>(4 * block.size()))
                ok = false, step = "size";
            else if (::pread(fd, back.data(), back.size(), static_cast<off_t>(block.size())) !=
                         static_cast<ssize_t>(back.size()) ||
                     back != block)
                ok = false, step = "read";
            ::close(fd);
        }
    }
    if (ok && ::rename(part.c_str(), done.c_str()) != 0)
        ok = false, step = "rename";
    if (!ok)
        fail(step);
    // Cleaning up must work too: a cancelled copy removes what it wrote.
    const bool removed = ::unlink(ok ? done.c_str() : part.c_str()) == 0;
    if (ok && !removed)
    {
        fail("remove");
        ok = false;
    }
    if (::rmdir(folder.c_str()) != 0 && ok)
    {
        fail("remove folder");
        ok = false;
    }
    if (ok)
    {
        destination->writable = 1;
        destination->why.clear();
    }
}

struct StorageWatcher::Shared
{
    std::mutex lock;
    StorageSpace latest;
    std::atomic<bool> stop{false};
    std::atomic<bool> again{false};
    std::uint64_t interval_ms = 10000;
    int ui_cpu = -1;
};

namespace
{

void *watch(void *argument)
{
    // The thread keeps the shared state alive by itself; the watcher may be
    // gone long before a slow request returns.
    auto *owned = static_cast<std::shared_ptr<StorageWatcher::Shared> *>(argument);
    std::shared_ptr<StorageWatcher::Shared> shared = std::move(*owned);
    delete owned;
#if defined(__PROSPERO__)
    // Off the UI's core: console threads aren't time-sliced.
    if (shared->ui_cpu >= 0)
    {
        cpuset_t allowed;
        CPU_ZERO(&allowed);
        if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) == 0 &&
            CPU_ISSET(shared->ui_cpu, &allowed))
        {
            CPU_CLR(shared->ui_cpu, &allowed);
            if (!CPU_EMPTY(&allowed))
                pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed);
        }
    }
#endif
    std::vector<Destination> checked;
    while (!shared->stop.load())
    {
        StorageSpace space = shadowmount_storage();
        if (!space.known)
        {
            const std::string why = space.how;
            const StorageSpace answer = std::move(space);
            space = internal_storage();
            space.asked = true;
            if (!answer.destinations.empty())
                space.destinations = answer.destinations;
            if (answer.drives_known)
            {
                space.drives_known = true;
                space.drives = answer.drives;
                space.drive_space = answer.drive_space;
            }
            if (!space.known)
                space.how = why + " | " + space.how;
        }
        // The drives plugged in: ShadowMountPlus's mount table, or (when it
        // doesn't answer, or lists none) what NASBridge sees itself.
        std::vector<std::string> seen_drives;
        if (space.drives.empty() && probe_drives(&seen_drives))
        {
            space.drives = seen_drives;
            space.drives_known = true;
        }
        else if (!space.drives_known)
        {
            // Nobody could tell this time: the last answer stands, so a
            // drive doesn't seem unplugged while ShadowMountPlus is busy.
            std::lock_guard<std::mutex> hold(shared->lock);
            space.drives_known = shared->latest.drives_known;
            space.drives = shared->latest.drives;
            space.drive_space = shared->latest.drive_space;
        }
        merge_destinations(&space);
        // The games ShadowMountPlus knows. Kept from the last round when it
        // doesn't answer this time, so "On PS5" doesn't flicker.
        {
            std::string why;
            std::vector<SmpGame> games;
            if (smp_games(&games, &why))
            {
                space.games_known = true;
                space.games = std::move(games);
            }
            else
            {
                std::lock_guard<std::mutex> hold(shared->lock);
                space.games_known = shared->latest.games_known;
                space.games = shared->latest.games;
            }
        }
        // Each folder is checked once while it stays listed (a drive plugged
        // in again is checked again).
        std::vector<Destination> seen;
        for (Destination &destination : space.destinations)
        {
            const Destination *before = nullptr;
            for (const Destination &old : checked)
                if (old.path == destination.path && old.mount_point == destination.mount_point)
                    before = &old;
            if (before != nullptr)
            {
                destination.writable = before->writable;
                destination.why = before->why;
            }
            else if (destination.writable < 0 && !shared->stop.load())
            {
                check_destination(&destination);
            }
            seen.push_back(destination);
        }
        checked = std::move(seen);
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->latest = space;
        }
        // Wait for the next round, a refresh, or the end, in small steps.
        const std::uint64_t until = now_ms() + shared->interval_ms;
        while (!shared->stop.load() && !shared->again.exchange(false) && now_ms() < until)
        {
            timespec step{0, 100 * 1000 * 1000};
            nanosleep(&step, nullptr);
        }
    }
    return nullptr;
}

} // namespace

StorageWatcher::StorageWatcher(float seconds) : shared_(std::make_shared<Shared>())
{
    shared_->interval_ms = static_cast<std::uint64_t>(seconds * 1000.0f);
#if defined(__PROSPERO__)
    shared_->ui_cpu = sceKernelGetCurrentCpu();
#endif
    auto *argument = new std::shared_ptr<Shared>(shared_);
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, &watch, argument);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete argument;
        // No thread: ask once here (the answer that needs no network).
        StorageSpace space = internal_storage();
        space.asked = true;
        shared_->latest = space;
    }
}

StorageWatcher::~StorageWatcher()
{
    shared_->stop.store(true);
}

StorageSpace StorageWatcher::latest() const
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    return shared_->latest;
}

void StorageWatcher::refresh()
{
    shared_->again.store(true);
}

} // namespace hui::concepts
