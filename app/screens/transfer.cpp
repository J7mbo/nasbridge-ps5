// NASBridge - The copy queue, and deleting games from the PS5.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/transfer.hpp"

#include "concepts/copy_helper.hpp"
#include "concepts/delete_helper.hpp"
#include "concepts/exfat_image.hpp"
#include "concepts/file_calls.hpp"
#include "concepts/http.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas.hpp"
#include "concepts/notify.hpp"
#include "concepts/smp.hpp"
#include "concepts/storage.hpp"

#include "core/save_file.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__PROSPERO__)
#include <pthread_np.h>
#include <sys/cpuset.h>
extern "C" int sceKernelGetCurrentCpu(void);
#endif

namespace hui::concepts
{

namespace
{

constexpr const char *kIncoming = ".nasbridge-incoming";
constexpr std::size_t kWriteBlock = 8u * 1024u * 1024u;     // written to the drive at once
constexpr std::uint64_t kRefetch = 4ull * 1024ull * 1024ull; // re-read at a resumed file's end
// Files under this go to the copy helper in batches, batch_workers() at a
// time (bigger ones on their own, shared between helper_streams() connections).
constexpr std::uint64_t kBatchFileMax = 16ull * 1024ull * 1024ull;
constexpr int kRetries = 6;           // connection failures in a row before giving up
constexpr int kReplySeconds = 30;     // how long the NAS may go quiet mid-file
constexpr int kListFolders = 20000;   // folders walked when the NAS refuses deep listings
// Big files come down over several connections at once, a piece each: one
// connection's speed is capped by how much it may have on its way, which over
// Wi-Fi is far below the network's.
constexpr std::uint64_t kParallelMin = 64ull * 1024ull * 1024ull; // files this big and up
constexpr std::uint64_t kPiece = 32ull * 1024ull * 1024ull;       // one request's share
constexpr int kStreams = 4; // the most connections the code can run (array sizes)
// How many connections a copy actually uses, read once when a copy starts and
// clamped to 1..kStreams. One is best for a single spinning HDD on the NAS:
// extra connections just make the one disk head seek between each stream's
// region, which collapses read speed under sustained load (two streams roughly
// halve each other, four fall off a cliff). Raise it only for a NAS on SSD or
// striped disks, where concurrent reads are free. set_copy_streams() changes it.
std::atomic<int> g_copy_streams{1};
constexpr std::size_t kPieceBuffer = 4u * 1024u * 1024u;          // written to the drive at once
constexpr const char *kPartSuffix = ".nbpart"; // a big file while its pieces come in
constexpr int kRejoinSeconds = 30;   // longest wait before a dropped connection tries again
constexpr int kStalledSeconds = 120; // connections give up when nothing arrived for this long
// A copy's file is flushed to the drive this often, or after this much, so
// unflushed writes never pile up to the PS5's dirty-buffer throttle.
constexpr std::uint64_t kSyncMs = 250;
constexpr std::uint64_t kSyncBytes = 64ull * 1024ull * 1024ull;

std::string g_root; // set_transfer_root (tests); empty: data_root()
std::atomic<bool> g_as_image{true}; // set_copy_as_image: folder games become images

// Which pieces of the big file being copied are in: one file at a time.
std::string part_record()
{
    return (g_root.empty() ? data_root() : g_root) + "/part.txt";
}

bool load_part(const std::string &entry, const std::string &path, std::uint64_t size,
               std::vector<char> *done)
{
    std::string text;
    if (!save::read_file(part_record(), &text, 1u << 20))
        return false;
    const std::string head = "nasbridge-part 1\n" + entry + "\t" + path + "\t" +
                             std::to_string(size) + "\t";
    if (text.compare(0, head.size(), head) != 0)
        return false;
    const std::string bits = text.substr(head.size(), done->size());
    if (bits.size() != done->size())
        return false;
    for (std::size_t i = 0; i < bits.size(); ++i)
        (*done)[i] = bits[i] == '1' ? 1 : 0;
    return true;
}

void save_part(const std::string &entry, const std::string &path, std::uint64_t size,
               const std::vector<char> &done)
{
    std::string bits;
    for (const char piece : done)
        bits += piece ? '1' : '0';
    save::write_atomic(part_record(), "nasbridge-part 1\n" + entry + "\t" + path + "\t" +
                                          std::to_string(size) + "\t" + bits + "\n");
}

void forget_part(const std::string &entry)
{
    std::string text;
    if (save::read_file(part_record(), &text, 1u << 20) &&
        text.compare(0, 17 + entry.size() + 1, "nasbridge-part 1\n" + entry + "\t") == 0)
        ::unlink(part_record().c_str());
}

std::uint64_t now_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

std::uint64_t now_ns()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(now.tv_nsec);
}

void sleep_ms(int ms)
{
    timespec step{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&step, nullptr);
}


// How many files the delete helper deletes at once for a folder game: on the
// PS5's internal storage each delete waits milliseconds on the drive, and
// the waits overlap: tools/deletebench measured 189 files a second one at a
// time, 503 at 8, and no more at 16 or 32, whatever the files' size.
int g_delete_threads = 8;

// How long a deep listing may stay silent before the game is listed folder by
// folder instead. A NAS that builds the whole listing before sending any of
// it, or ignores the request, would otherwise hold the copy up for a minute.
int g_deep_wait_s = 8;

// NASes (nas_identity) that didn't list deeply, this run.
std::mutex g_shallow_lock;
std::vector<std::string> g_shallow;

bool shallow_nas(const std::string &nas)
{
    std::lock_guard<std::mutex> hold(g_shallow_lock);
    return std::find(g_shallow.begin(), g_shallow.end(), nas) != g_shallow.end();
}

void remember_shallow_nas(const std::string &nas)
{
    std::lock_guard<std::mutex> hold(g_shallow_lock);
    if (std::find(g_shallow.begin(), g_shallow.end(), nas) == g_shallow.end())
        g_shallow.push_back(nas);
}

std::string errno_text(const char *what, int error)
{
    char text[160];
    if (error == ENOSPC)
        std::snprintf(text, sizeof(text), "The drive is full");
    else
        std::snprintf(text, sizeof(text), "%s (error %d)", what, error);
    return text;
}

bool is_archive(const std::string &name)
{
    const std::string low = lowered(name);
    const std::size_t dot = low.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string ext = low.substr(dot + 1);
    if (ext == "zip" || ext == "rar" || ext == "7z")
        return true;
    // Split parts: .z01, .r00, .7z.001, .zip.001, .part1.rar (caught above).
    if (ext.size() == 3 && (ext[0] == 'z' || ext[0] == 'r') && ext[1] >= '0' && ext[1] <= '9' &&
        ext[2] >= '0' && ext[2] <= '9')
        return true;
    if (ext.size() == 3 && ext[0] >= '0' && ext[0] <= '9' && ext[1] >= '0' && ext[1] <= '9' &&
        ext[2] >= '0' && ext[2] <= '9')
    {
        const std::string before = low.substr(0, dot);
        return ends_with_nocase(before, ".7z") || ends_with_nocase(before, ".zip") ||
               ends_with_nocase(before, ".rar");
    }
    return false;
}

// A relative path NASBridge is willing to create: no "..", no hidden or
// NAS-internal parts, nothing that would break the saved plan.
bool safe_relative(const std::string &path)
{
    if (path.empty() || path.front() == '/' || path.find('\t') != std::string::npos ||
        path.find('\n') != std::string::npos || path.find('\r') != std::string::npos)
        return false;
    std::size_t start = 0;
    while (start <= path.size())
    {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part[0] == '.' || part[0] == '@' ||
            part[0] == '#')
            return false;
        start = end + 1;
    }
    return true;
}

bool safe_name(const std::string &name)
{
    return safe_relative(name) && name.find('/') == std::string::npos;
}

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Makes `path` and its parents under `base` (which exists). False with errno.
bool make_folders(const std::string &base, const std::string &relative)
{
    std::size_t at = 0;
    while (at < relative.size())
    {
        std::size_t end = relative.find('/', at);
        if (end == std::string::npos)
            end = relative.size();
        const std::string folder = base + "/" + relative.substr(0, end);
        if (::mkdir(folder.c_str(), 0777) != 0 && errno != EEXIST)
            return false;
        at = end + 1;
    }
    return true;
}

// Whether something is at `path`. NASBridge can't stat in its sandbox, so it
// tries to open it, then to make a folder there.
bool exists(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    if (errno == ENOENT)
        return false;
    if (::mkdir(path.c_str(), 0777) == 0)
    {
        ::rmdir(path.c_str());
        return false;
    }
    return errno == EEXIST;
}

void keep_off_cpu(int avoid)
{
#if defined(__PROSPERO__)
    if (avoid < 0)
        return;
    cpuset_t allowed;
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0 ||
        !CPU_ISSET(avoid, &allowed))
        return;
    CPU_CLR(avoid, &allowed);
    if (!CPU_EMPTY(&allowed))
        pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed);
#else
    (void)avoid;
#endif
}

int current_cpu()
{
#if defined(__PROSPERO__)
    return sceKernelGetCurrentCpu();
#else
    return -1;
#endif
}

} // namespace

// ---- the saved plan ----

std::string nas_identity(const Connection &connection)
{
    // WebDAV keeps its old form, so copies saved by earlier versions still match.
    const std::string where = connection.host + ":" +
                              std::to_string(connection.effective_port()) + folder_path(connection);
    return connection.type == ConnectionType::smb ? "smb:" + where : where;
}

std::string copy_source(const Connection &connection, const NasGame &game)
{
    const std::string root = folder_path(connection);
    if (game.image)
        return root + game.image_path;
    // The folder holding sce_sys; the game's own folder when sce_sys is in it.
    const std::string holder = parent_of(game.base);
    return root + (holder.empty() ? game.entry : holder) + "/";
}

std::string copy_name(const NasGame &game)
{
    if (game.image)
    {
        const std::size_t slash = game.image_path.rfind('/');
        return slash == std::string::npos ? game.image_path : game.image_path.substr(slash + 1);
    }
    return game.entry;
}

void set_transfer_root(const std::string &root)
{
    g_root = root;
}

void set_delete_threads(int threads)
{
    g_delete_threads = threads < 1 ? 1 : threads > 32 ? 32 : threads;
}

void set_deep_listing_wait(int seconds)
{
    g_deep_wait_s = seconds;
    std::lock_guard<std::mutex> hold(g_shallow_lock);
    g_shallow.clear();
}


void set_copy_streams(int streams)
{
    g_copy_streams.store(streams < 1 ? 1 : (streams > kStreams ? kStreams : streams));
}

void set_copy_as_image(bool on)
{
    g_as_image.store(on);
}

bool copy_as_image()
{
    return g_as_image.load();
}

std::uint64_t image_estimate(std::uint64_t bytes)
{
    // The room kept free inside (exfat_image.hpp: 0.5%, 64 MiB to 512 MiB),
    // what each file wastes of its last cluster (64 KiB; some 32 KiB a file
    // on average, taken as 1%), and the file system's own few MB.
    const std::uint64_t spare = std::min<std::uint64_t>(std::max<std::uint64_t>(bytes / 200, 64ull << 20), 512ull << 20);
    return bytes + bytes / 100 + spare + (16ull << 20);
}

std::string plan_to_text(const CopyPlan &plan)
{
    std::string text = "nasbridge-copy 1\n";
    const auto line = [&](const char *key, const std::string &value)
    { text += std::string(key) + "\t" + value + "\n"; };
    line("entry", plan.entry);
    line("title", plan.title);
    line("title_id", plan.title_id);
    line("image", plan.image ? "1" : "0");
    if (plan.as_image)
    {
        line("as_image", "1");
        line("image_size", std::to_string(plan.image_size));
        line("image_layout", std::to_string(plan.image_layout));
    }
    line("source", plan.source);
    line("destination", plan.destination);
    line("name", plan.name);
    line("nas", plan.nas);
    line("listed", plan.listed ? "1" : "0");
    line("moving", plan.moving ? "1" : "0");
    for (const std::string &folder : plan.folders)
        line("d", folder);
    for (const CopyFile &file : plan.files)
        text += "f\t" + std::to_string(file.size) + "\t" + file.path + "\n";
    return text;
}

bool plan_from_text(const std::string &text, CopyPlan *plan)
{
    *plan = CopyPlan{};
    std::size_t at = 0;
    bool header = false;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!header)
        {
            if (line != "nasbridge-copy 1")
                return false;
            header = true;
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        const std::string key = line.substr(0, tab);
        const std::string value = line.substr(tab + 1);
        if (key == "entry")
            plan->entry = value;
        else if (key == "title")
            plan->title = value;
        else if (key == "title_id")
            plan->title_id = value;
        else if (key == "image")
            plan->image = value == "1";
        else if (key == "as_image")
            plan->as_image = value == "1";
        else if (key == "image_size")
            plan->image_size = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "image_layout")
            plan->image_layout = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "source")
            plan->source = value;
        else if (key == "destination")
            plan->destination = value;
        else if (key == "name")
            plan->name = value;
        else if (key == "nas")
            plan->nas = value;
        else if (key == "listed")
            plan->listed = value == "1";
        else if (key == "moving")
            plan->moving = value == "1";
        else if (key == "d")
            plan->folders.push_back(value);
        else if (key == "f")
        {
            const std::size_t second = value.find('\t');
            if (second == std::string::npos)
                return false;
            CopyFile file;
            file.size = std::strtoull(value.substr(0, second).c_str(), nullptr, 10);
            file.path = value.substr(second + 1);
            plan->files.push_back(file);
        }
    }
    for (const CopyFile &file : plan->files)
        plan->total += file.size;
    return header && !plan->entry.empty() && !plan->destination.empty() && !plan->name.empty() &&
           !plan->source.empty();
}

// ---- the saved queue ----

std::string queue_to_text(const std::vector<SavedItem> &items)
{
    std::string text = "nasbridge-queue 1\n";
    for (const SavedItem &item : items)
    {
        text += "item\t" + std::to_string(item.estimate) + "\t" + std::to_string(item.done) + "\t" +
                (item.started ? "1" : "0") + "\n";
        text += plan_to_text(item.plan);
        text += "end\n";
    }
    return text;
}

bool queue_from_text(const std::string &text, std::vector<SavedItem> *items)
{
    items->clear();
    if (text.compare(0, 18, "nasbridge-queue 1\n") != 0)
        return false;
    std::size_t at = 18;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line.compare(0, 5, "item\t") != 0)
            continue;
        SavedItem item;
        unsigned long long estimate = 0;
        unsigned long long done = 0;
        int started = 0;
        if (std::sscanf(line.c_str() + 5, "%llu\t%llu\t%d", &estimate, &done, &started) != 3)
            return false;
        item.estimate = estimate;
        item.done = done;
        item.started = started != 0;
        const std::size_t stop = text.find("\nend\n", at - 1);
        if (stop == std::string::npos)
            return false;
        if (!plan_from_text(text.substr(at, stop + 1 - at), &item.plan))
            return false;
        at = stop + 5;
        items->push_back(std::move(item));
    }
    return true;
}

// ---- the shared state ----

namespace
{

std::string queue_file()
{
    return (g_root.empty() ? data_root() : g_root) + "/queue.txt";
}

std::string unfinished_file()
{
    return (g_root.empty() ? data_root() : g_root) + "/deleting.txt";
}

double gigabytes(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / 1e9;
}

} // namespace

struct Item
{
    CopyPlan plan;
    std::uint64_t estimate = 0; // bytes, until the plan lists the files
    std::uint64_t done = 0;
    bool started = false;

    std::uint64_t size() const
    {
        if (plan.as_image)
            return plan.listed && plan.image_size > 0 ? plan.image_size
                                                     : image_estimate(plan.listed ? plan.total : estimate);
        return plan.listed ? plan.total : estimate;
    }
};

struct Transfers::Shared
{
    mutable std::mutex lock;
    std::vector<Item> queue;
    QueueState state = QueueState::idle;
    std::string message;
    TransferState phase = TransferState::idle;
    std::string detail;
    double rate = 0.0;
    int connections = 0; // at work right now (see Pieces::active)
    std::string conn_report; // a per-connection line, while a big file copies
    int files_done = 0;
    int files_total = 0;
    std::vector<std::string> discarding;
    std::map<std::string, float> discarded;
    std::string finished_entry;
    std::string finished_destination;
    std::vector<QueueStatus::Waiting> waiting_for_close;
    bool worker = false;            // the queue's thread is running
    Connection connection;          // the NAS the queue copies from
    TransferStatus removal;         // deleting a game on the PS5
    // A folder game ShadowMountPlus removed from the home screen whose files
    // weren't all deleted (the helper stopped): they're hidden in
    // <leftover_root>/.nasbridge-deleting, and "Delete the rest" finishes it.
    std::string leftover_title;
    std::string leftover_root;
    // Folders games go to whose .nasbridge-deleting may still hold files
    // (saved: what a delete cut short leaves is finished at the next start).
    std::vector<std::string> unfinished;
    bool finishing = false; // finishing those, at start-up
    void save_unfinished()
    {
        std::string text;
        for (const std::string &root : unfinished)
            text += root + "\n";
        if (text.empty())
            ::unlink(unfinished_file().c_str());
        else
            save::write_atomic(unfinished_file(), text);
    }
    void add_unfinished(const std::string &root)
    {
        if (std::find(unfinished.begin(), unfinished.end(), root) == unfinished.end())
        {
            unfinished.push_back(root);
            save_unfinished();
        }
    }
    void drop_unfinished(const std::string &root)
    {
        unfinished.erase(std::remove(unfinished.begin(), unfinished.end(), root), unfinished.end());
        save_unfinished();
    }
    Transfers::SpaceCheck space;
    std::atomic<bool> pause{false}; // the screen asked the queue to stop
    std::atomic<bool> stop{false};  // NASBridge is closing
    int ui_cpu = -1;

    // The queued game called `entry` (lock held), or null.
    Item *item(const std::string &entry)
    {
        for (Item &candidate : queue)
            if (candidate.plan.entry == entry)
                return &candidate;
        return nullptr;
    }

    // Writes the queue to the PS5's storage (lock held).
    void save() const
    {
        std::vector<SavedItem> items;
        for (const Item &queued : queue)
            items.push_back({queued.plan, queued.estimate, queued.done, queued.started});
        if (items.empty())
            ::unlink(queue_file().c_str());
        else
            save::write_atomic(queue_file(), queue_to_text(items));
    }

    void set_phase(TransferState now, const std::string &text = std::string())
    {
        std::lock_guard<std::mutex> hold(lock);
        phase = now;
        detail = text;
        if (now != TransferState::copying)
            rate = 0.0;
    }

    void set_removal(TransferState next, const std::string &text = std::string())
    {
        TransferStatus now;
        {
            std::lock_guard<std::mutex> hold(lock);
            removal.state = next;
            removal.message = text;
            now = removal;
        }
        // A PS5 notification when a delete ends.
        if (next == TransferState::deleted)
            ps5_notify(now.title + " was deleted from the PS5.");
        else if (next == TransferState::failed)
            ps5_notify("Couldn't delete " + now.title + ": " + text);
    }
};

namespace
{

using SharedPtr = std::shared_ptr<Transfers::Shared>;

// ---- listing the game on the NAS ----

class Lister
{
  public:
    Lister(const Connection &connection, const SharedPtr &shared)
        : connection_(connection), shared_(shared), nas_(connection, kReplySeconds)
    {
    }

    // Fills plan->folders and plan->files. False with *why.
    bool list(CopyPlan *plan, std::string *why)
    {
        if (plan->image)
            return list_image(plan, why);
        root_ = plan->source;
        while (root_.size() > 1 && root_.back() == '/')
            root_.pop_back();
        plan->folders.clear();
        plan->files.clear();
        // A NAS that didn't list deeply once isn't asked again (it can cost
        // the whole wait each time). SMB has no deep listing.
        const std::string nas = nas_identity(connection_);
        const bool try_deep = !nas_.smb() && !shallow_nas(nas);
        if (!try_deep || !deep(plan))
        {
            if (try_deep && !stopping())
                remember_shallow_nas(nas);
            if (!walk(plan, why))
                return false;
        }
        if (plan->files.empty())
        {
            *why = "The game's folder on the NAS is empty";
            return false;
        }
        // Parents before children, so they can be made in order.
        std::sort(plan->folders.begin(), plan->folders.end());
        plan->total = 0;
        for (const CopyFile &file : plan->files)
            plan->total += file.size;
        plan->listed = true;
        return true;
    }

  private:
    bool stopping() const
    {
        return shared_->pause.load() || shared_->stop.load();
    }

    // "Listing the game's files: 1,240 found", at most a few times a second.
    void report(const CopyPlan &plan, bool now = false)
    {
        const std::uint64_t at = now_ms();
        if (!now && at < reported_ + 250)
            return;
        reported_ = at;
        std::string count = std::to_string(plan.files.size());
        for (int cut = static_cast<int>(count.size()) - 3; cut > 0; cut -= 3)
            count.insert(static_cast<std::size_t>(cut), ",");
        shared_->set_phase(TransferState::preparing, "Listing the game's files: " + count + " found");
    }

    // An entry of the listing, as a path relative to the game. True for a
    // folder that was taken (the walk lists it next).
    bool add(const DavEntry &entry, CopyPlan *plan)
    {
        if (entry.path.size() <= root_.size() + 1 ||
            entry.path.compare(0, root_.size() + 1, root_ + "/") != 0)
            return false; // the folder itself, or not under it
        const std::string relative = entry.path.substr(root_.size() + 1);
        if (!safe_relative(relative))
            return false; // hidden, NAS-internal, or a name NASBridge won't write
        if (entry.folder)
        {
            plan->folders.push_back(relative);
            return true;
        }
        else if (!is_archive(entry.name))
        {
            CopyFile file;
            file.path = relative;
            file.size = entry.size;
            plan->files.push_back(file);
        }
        return false;
    }

    // One "Depth: infinity" listing, read as it streams in (big games list
    // tens of megabytes of XML).
    bool deep(CopyPlan *plan)
    {
        std::string pending;
        bool failed = false;
        const std::string url = root_ + "/";
        const auto take = [&](bool final)
        {
            const std::size_t at = pending.rfind("response>");
            std::size_t cut = std::string::npos;
            for (std::size_t look = at; look != std::string::npos;
                 look = look == 0 ? std::string::npos : pending.rfind("response>", look - 1))
            {
                const std::size_t open = pending.rfind('<', look);
                if (open != std::string::npos && open + 1 < pending.size() &&
                    pending[open + 1] == '/')
                {
                    cut = look + 9;
                    break;
                }
            }
            if (cut == std::string::npos)
            {
                if (final)
                    pending.clear();
                return;
            }
            std::vector<DavEntry> entries;
            parse_listing(pending.substr(0, cut), url, &entries);
            for (const DavEntry &entry : entries)
                add(entry, plan);
            pending.erase(0, cut);
            report(*plan);
        };
        const HttpReply reply = http_stream(
            connection_, "PROPFIND", url,
            "Depth: infinity\r\nContent-Type: application/xml; charset=utf-8\r\n", propfind_body(),
            [&](const char *data, std::size_t length)
            {
                if (stopping())
                    return false;
                pending.append(data, length);
                if (pending.size() >= 256 * 1024)
                    take(false);
                if (pending.size() > 8u * 1024u * 1024u)
                {
                    failed = true;
                    return false;
                }
                return true;
            },
            g_deep_wait_s, nullptr, [this] { return stopping(); });
        if (failed || reply.status != 207 || !reply.complete)
        {
            plan->folders.clear();
            plan->files.clear();
            return false;
        }
        take(true);
        return true;
    }

    // One folder at a time, when the NAS won't list deeply. Over SMB, several
    // folders at once.
    bool walk(CopyPlan *plan, std::string *why)
    {
        if (nas_.smb())
            return walk_smb(plan, why);
        std::vector<std::string> queue{root_ + "/"};
        for (std::size_t next = 0; next < queue.size(); ++next)
        {
            if (stopping())
            {
                *why = "Stopped";
                return false;
            }
            if (static_cast<int>(queue.size()) > kListFolders)
            {
                *why = "The game has too many folders to list";
                return false;
            }
            std::vector<DavEntry> entries;
            NasProblem problem;
            if (!nas_.list(queue[next], &entries, &problem))
            {
                *why = problem.reached ? "The NAS wouldn't list " + queue[next] + " (" +
                                             problem.brief + ")"
                                       : "Lost the connection to the NAS while listing";
                return false;
            }
            for (const DavEntry &entry : entries)
                if (add(entry, plan))
                    queue.push_back(entry.path + "/");
            report(*plan);
        }
        return true;
    }

    bool walk_smb(CopyPlan *plan, std::string *why)
    {
        int folders = 0;
        bool too_many = false;
        NasProblem problem;
        const bool ok = nas_.walk_smb(
            root_ + "/",
            [&](const std::string &, const std::vector<DavEntry> &entries)
            {
                if (stopping())
                    return false;
                if (++folders > kListFolders)
                {
                    too_many = true;
                    return false;
                }
                for (const DavEntry &entry : entries)
                    add(entry, plan);
                report(*plan);
                return true;
            },
            &problem);
        if (ok)
            return true;
        if (too_many)
            *why = "The game has too many folders to list";
        else if (problem.brief.empty())
            *why = "Stopped";
        else
            *why = problem.reached ? "The NAS wouldn't list the game's folders (" + problem.brief + ")"
                                   : "Lost the connection to the NAS while listing";
        return false;
    }

    // An image: one file; its size from its folder's listing.
    bool list_image(CopyPlan *plan, std::string *why)
    {
        const std::string folder = parent_of(plan->source) + "/";
        std::vector<DavEntry> entries;
        NasProblem problem;
        if (!nas_.list(folder, &entries, &problem))
        {
            *why = problem.reached ? "The NAS wouldn't list the image's folder (" + problem.brief + ")"
                                   : "Couldn't reach the NAS";
            return false;
        }
        for (const DavEntry &entry : entries)
        {
            if (!entry.folder && entry.path == plan->source)
            {
                plan->folders.clear();
                plan->files = {CopyFile{"", entry.size}};
                plan->total = entry.size;
                plan->listed = true;
                return true;
            }
        }
        *why = "The image isn't on the NAS any more";
        return false;
    }

    Connection connection_;
    SharedPtr shared_;
    NasReader nas_;
    std::string root_; // the game's folder, decoded, without the trailing slash
    std::uint64_t reported_ = 0;
};

// ---- copying one game (the front of the queue) ----

enum class Outcome
{
    done,
    paused,  // the screen paused the queue
    closing, // NASBridge is closing: the saved queue brings it back
    failed,
};

class Copier
{
  public:
    Copier(const Connection &connection, const SharedPtr &shared, const std::string &entry)
        : connection_(connection), shared_(shared), entry_(entry)
    {
    }

    Outcome run(std::string *why)
    {
        CopyPlan plan;
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            const Item *item = shared_->item(entry_);
            if (item == nullptr)
            {
                *why = "The game left the queue";
                return Outcome::failed;
            }
            plan = item->plan;
            shared_->files_total = static_cast<int>(plan.files.size());
            shared_->files_done = 0;
        }
        const std::string incoming = plan.destination + "/" + kIncoming;
        staging_ = incoming + "/" + plan.name;

        // 1. The list of files (once; a resumed copy has it).
        if (!plan.listed)
        {
            shared_->set_phase(TransferState::preparing, "Listing the game's files");
            Lister lister(connection_, shared_);
            if (!lister.list(&plan, why))
                return stopped_or(Outcome::failed);
            std::lock_guard<std::mutex> hold(shared_->lock);
            if (Item *item = shared_->item(entry_))
                item->plan = plan;
            shared_->files_total = static_cast<int>(plan.files.size());
            shared_->save();
        }

        // A folder game as one exFAT image: laid out from the list, the same
        // way every time, so a resumed copy lays it out again rather than
        // keeping it. (A different layout, from another NASBridge version,
        // starts the image again: what's there wouldn't fit it.)
        ExfatImage image;
        if (plan.as_image)
        {
            std::string problem;
            if (!plan_exfat_image(plan.folders, image_files(plan),
                                  plan.title_id.size() <= 11 ? plan.title_id : std::string(), &image,
                                  &problem))
                return as_folder(plan, problem, why);
            const std::uint64_t layout = layout_of(image);
            if (layout != plan.image_layout || image.size != plan.image_size)
            {
                if (plan.image_layout != 0 && !plan.moving)
                    ::unlink(staging_.c_str());
                plan.image_size = image.size;
                plan.image_layout = layout;
                std::lock_guard<std::mutex> hold(shared_->lock);
                if (Item *item = shared_->item(entry_))
                {
                    item->plan = plan;
                    item->done = 0;
                }
                shared_->save();
            }
            // The queue checked the room against an estimate; now the size is
            // known (a game of many small files wastes more of each cluster).
            struct stat staged{};
            const std::uint64_t have =
                ::stat(staging_.c_str(), &staged) == 0 ? static_cast<std::uint64_t>(staged.st_size) : 0;
            const std::uint64_t need = image.size > have ? image.size - have : 0;
            std::uint64_t free = 0;
            if (shared_->space && shared_->space(plan.destination, &free) && need > free)
            {
                char text[200];
                std::snprintf(text, sizeof(text),
                              "Not enough space on %s for %s as an exFAT image: it needs %.1f GB, %.1f GB is free",
                              destination_name(plan.destination).c_str(), plan.title.c_str(),
                              gigabytes(need), gigabytes(free));
                return fail(text, why);
            }
        }

        // 2. Somewhere to put it: not over a game of the same name. (When an
        //    earlier run had begun moving the game into place and nothing is
        //    left in the staging folder, the move happened: only the rescan
        //    is left.)
        if (exists(plan.destination + "/" + plan.name))
        {
            if (plan.moving && !exists(staging_))
                return finish(incoming, why);
            *why = "There's already a \"" + plan.name + "\" on this drive";
            return stopped_or(Outcome::failed);
        }
        if (::mkdir(incoming.c_str(), 0777) != 0 && errno != EEXIST)
            return fail(errno_text("Couldn't write to the drive", errno), why);
        if (!plan.one_file() && ::mkdir(staging_.c_str(), 0777) != 0 && errno != EEXIST)
            return fail(errno_text("Couldn't write to the drive", errno), why);
        {
            // From here it has files on the drive: removing it deletes them.
            std::lock_guard<std::mutex> hold(shared_->lock);
            if (Item *item = shared_->item(entry_))
                item->started = true;
            shared_->save();
        }
        for (const std::string &folder : plan.folders)
            if (!plan.as_image && !make_folders(staging_, folder))
                return fail(errno_text(("Couldn't make " + folder).c_str(), errno), why);

        // 3. Each file, carrying on from what is already there.
        shared_->set_phase(TransferState::copying);
        buffer_.reserve(kWriteBlock + 64 * 1024); // a block plus one read
        std::uint64_t done = 0;
        int files_done = 0;
        if (plan.as_image)
        {
            const Result result = copy_image(plan, image, why);
            if (result == Result::fallback)
                return as_folder(plan, *why, why);
            if (result == Result::stopped)
                return stopped_or(Outcome::paused);
            if (result != Result::ok)
                return stopped_or(Outcome::failed);
            if (!check_image(plan, why))
                return stopped_or(Outcome::failed);
            done = image.size;
            files_done = static_cast<int>(plan.files.size());
        }
        else
        {
            for (std::size_t index = 0; index < plan.files.size();)
            {
                // A run of small files goes to the copy helper in one batch (one
                // request at a time costs more than the files themselves).
                if (!plan.image && batch_ok_ && copy_helper_enabled() && !helper_unavailable_ &&
                    plan.files[index].size < kBatchFileMax)
                {
                    std::size_t end = index;
                    while (end < plan.files.size() && plan.files[end].size < kBatchFileMax)
                        ++end;
                    const Result result = copy_batch(plan, index, end, &done, &files_done, why);
                    if (result == Result::stopped)
                        return stopped_or(Outcome::paused);
                    if (result == Result::failed)
                        return stopped_or(Outcome::failed);
                    if (result == Result::ok)
                        index = end;
                    else
                        batch_ok_ = false; // an older helper: one at a time from here
                    continue;
                }
                const CopyFile &file = plan.files[index];
                ++index;
                const std::string local = plan.image ? staging_ : staging_ + "/" + file.path;
                const std::string remote = plan.image ? plan.source : plan.source + file.path;
                {
                    std::lock_guard<std::mutex> hold(shared_->lock);
                    if (Item *item = shared_->item(entry_))
                        item->done = done;
                    shared_->files_done = files_done;
                }
                const Result result = copy_file(remote, local, file.size, done, file.path, why);
                if (result == Result::stopped)
                    return stopped_or(Outcome::paused);
                if (result == Result::failed)
                    return stopped_or(Outcome::failed);
                done += file.size;
                ++files_done;
            }
        }
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            if (Item *item = shared_->item(entry_))
                item->done = done;
            shared_->files_done = files_done;
        }

        // 4. Into place, then ShadowMountPlus looks at once.
        shared_->set_phase(TransferState::finishing, "Adding the game to the home screen");
        const std::string final_path = plan.destination + "/" + plan.name;
        plan.moving = true;
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            if (Item *item = shared_->item(entry_))
                item->plan = plan;
            shared_->save();
        }
        if (::rename(staging_.c_str(), final_path.c_str()) != 0)
            return fail(errno_text("Couldn't move the game into place", errno), why);
        return finish(incoming, why);
    }

  private:
    enum class Result
    {
        ok,
        stopped,  // paused, or NASBridge closing
        failed,
        fallback, // the NAS won't send pieces: copy it in one go
    };

    // The game is in place: tidy up and have ShadowMountPlus look for it.
    Outcome finish(const std::string &incoming, std::string *why)
    {
        ::rmdir(incoming.c_str()); // only when nothing else is in it
        // From 1.7beta6, ShadowMountPlus scans while a homebrew app (a LAPY
        // title ID, as NASBridge has) is open: this adds the game, or, if
        // its files are too fresh for this scan (it waits stability_wait_s,
        // 10 s by default, in case they're still being written), its own
        // next scan does. An older one takes the scan but waits for NASBridge
        // to close ("game_active").
        std::string scan_why;
        std::string deferred;
        if (!smp_scan(true, &scan_why, &deferred))
            *why = "ShadowMountPlus wasn't asked to look for the game: " + scan_why;
        else if (deferred == "game_active")
            held_back_ = true;
        return Outcome::done;
    }

  public:
    // ShadowMountPlus took the scan but waits for NASBridge to close.
    bool held_back() const
    {
        return held_back_;
    }

  private:
    bool held_back_ = false;

    // A folder game that can't be an exFAT image (`reason`) is copied as a
    // folder instead, as if the setting were off: the half image goes, and
    // the copy starts again from the file list it already has.
    Outcome as_folder(CopyPlan plan, const std::string &reason, std::string *why)
    {
        if (plan.image_layout != 0 || exists(staging_))
            ::unlink(staging_.c_str());
        const std::string suffix = ".exfat";
        plan.as_image = false;
        if (plan.name.size() > suffix.size() &&
            plan.name.compare(plan.name.size() - suffix.size(), suffix.size(), suffix) == 0)
            plan.name.resize(plan.name.size() - suffix.size());
        plan.image_size = 0;
        plan.image_layout = 0;
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            for (const Item &queued : shared_->queue)
                if (queued.plan.entry != entry_ && queued.plan.destination == plan.destination &&
                    queued.plan.name == plan.name)
                    return fail(plan.title + " can't be an exFAT image (" + reason +
                                    "), and a game called \"" + plan.name +
                                    "\" is already queued for this drive",
                                why);
            if (Item *item = shared_->item(entry_))
            {
                item->plan = plan;
                item->done = 0;
            }
            shared_->save();
        }
        ps5_notify(plan.title + " can't be an exFAT image (" + reason + "), so it's copied as a folder.");
        return run(why);
    }

    Outcome fail(const std::string &text, std::string *why)
    {
        *why = text;
        return stopped_or(Outcome::failed);
    }

    bool stopping() const
    {
        return shared_->pause.load() || shared_->stop.load();
    }

    // A stop asked for by the screen (or NASBridge closing) wins over an
    // error it caused.
    Outcome stopped_or(Outcome outcome) const
    {
        if (shared_->stop.load())
            return Outcome::closing;
        if (shared_->pause.load())
            return Outcome::paused;
        return outcome;
    }

    // Writes what's buffered; false with errno on a write error.
    bool flush(int fd)
    {
        std::size_t written = 0;
        while (written < buffer_.size())
        {
            const ssize_t n = ::write(fd, buffer_.data() + written, buffer_.size() - written);
            if (n <= 0)
            {
                if (n < 0 && errno == EINTR)
                    continue;
                if (n == 0)
                    errno = EIO;
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        buffer_.clear();
        // Flushed little and often, as in copy_pieces (the PS5's dirty-buffer
        // throttle otherwise collapses a long write to a few MB/s).
        unsynced_ += written;
        const std::uint64_t now = now_ms();
        if (unsynced_ >= kSyncBytes || now - last_sync_ms_ >= kSyncMs)
        {
            last_sync_ms_ = now;
            unsynced_ = 0;
            if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
                return false;
        }
        return true;
    }

    Result copy_file(const std::string &remote, const std::string &local, std::uint64_t size,
                     std::uint64_t before, const std::string &path, std::string *why)
    {
        // Through the copy helper payload, outside the sandbox (where a long
        // write to the drive keeps its speed); in-app only when it can't run.
        if (copy_helper_enabled() && !helper_unavailable_)
        {
            const Result result = copy_via_helper(remote, local, size, before, why);
            if (result != Result::fallback)
                return result;
        }
        if (size >= kParallelMin && !smb())
        {
            const Result result = copy_pieces(remote, local, size, before, path, why);
            if (result != Result::fallback)
                return result;
        }
        // With the execute bit: a game whose files lack it won't start (issue #4).
        const int fd = ::open(local.c_str(), O_RDWR | O_CREAT, 0777);
        if (fd < 0)
        {
            *why = errno_text("Couldn't create a file on the drive", errno);
            return Result::failed;
        }
        const Result result = smb() ? fill_smb(fd, remote, size, before, why)
                                    : fill(fd, remote, size, before, why);
        if (result == Result::ok && ::fsync(fd) != 0)
        {
            *why = errno_text("Couldn't finish writing a file", errno);
            ::close(fd);
            return Result::failed;
        }
        ::close(fd);
        return result;
    }

    // ---- many small files, through the copy helper payload ----

    void publish(std::uint64_t done, int files_done)
    {
        std::lock_guard<std::mutex> hold(shared_->lock);
        if (Item *item = shared_->item(entry_))
            item->done = done;
        shared_->files_done = files_done;
    }

    // plan.files[begin, end), all small. A file already whole (a resumed
    // copy) is left; the rest are fetched from their start. Result::fallback:
    // the helper copies one file at a time (an older one), or couldn't start.
    Result copy_batch(const CopyPlan &plan, std::size_t begin, std::size_t end,
                      std::uint64_t *done, int *files_done, std::string *why)
    {
        std::vector<std::size_t> todo;
        for (std::size_t i = begin; i < end; ++i)
        {
            const CopyFile &file = plan.files[i];
            const std::string local = staging_ + "/" + file.path;
            struct stat info;
            if (::stat(local.c_str(), &info) == 0 &&
                static_cast<std::uint64_t>(info.st_size) == file.size &&
                !exists(local + kPartSuffix))
            {
                *done += file.size;
                ++*files_done;
            }
            else
                todo.push_back(i);
        }
        publish(*done, *files_done);
        int failures = 0;
        while (!todo.empty())
        {
            if (stopping())
                return Result::stopped;
            std::vector<BatchFile> files;
            files.reserve(todo.size());
            for (const std::size_t i : todo)
            {
                const CopyFile &file = plan.files[i];
                BatchFile item;
                item.local = staging_ + "/" + file.path;
                item.size = file.size;
                // Pieces of it from an in-app copy: the helper writes it whole.
                if (exists(item.local + kPartSuffix))
                {
                    ::unlink((item.local + kPartSuffix).c_str());
                    forget_part(entry_);
                }
                if (smb())
                {
                    item.request = smb_location(plan.source + file.path).path;
                }
                else
                {
                    item.request = build_request(connection_, "GET", plan.source + file.path, "", "");
                    const std::size_t close_at = item.request.find("Connection: close\r\n");
                    if (close_at != std::string::npos)
                        item.request.replace(close_at, 17, "Connection: keep-alive");
                }
                files.push_back(std::move(item));
            }
            std::vector<char> got(todo.size(), 0);
            std::size_t got_count = 0;
            std::uint64_t last_bytes = 0;
            window_start_ = now_ms();
            window_bytes_ = 0;
            const auto file_done = [&](std::size_t k)
                {
                    if (got[k])
                        return;
                    got[k] = 1;
                    ++got_count;
                    *done += files[k].size;
                    ++*files_done;
                    publish(*done, *files_done);
                };
            const auto batch_progress = [&](std::uint64_t bytes, std::uint64_t count, int busy,
                                            std::uint64_t saving_ms, std::uint64_t elapsed_ms)
                {
                    measure(*done, bytes > last_bytes ? static_cast<std::size_t>(bytes - last_bytes) : 0);
                    last_bytes = bytes;
                    // Settings > Debug: how many are being fetched, how many
                    // a second, and the share of the time spent saving.
                    char report[96];
                    if (elapsed_ms > 0)
                        std::snprintf(report, sizeof(report),
                                      "helper  %d at once  %d files/s  saving %d%%", busy,
                                      static_cast<int>(count * 1000 / elapsed_ms),
                                      static_cast<int>(saving_ms * 100 / elapsed_ms));
                    else
                        std::snprintf(report, sizeof(report), "helper  %d files at once", busy);
                    std::lock_guard<std::mutex> hold(shared_->lock);
                    shared_->connections = busy > 0 ? busy : 1;
                    shared_->conn_report = report;
                };
            const auto batch_stopping = [this] { return stopping(); };
            const HelperBatch batch =
                smb() ? copy_smb_batch_with_helper(connection_.host, connection_.effective_port(),
                                                   batch_workers(), sign_in(), files, file_done,
                                                   nullptr, batch_progress, batch_stopping)
                      : copy_batch_with_helper(connection_.host, connection_.effective_port(),
                                               batch_workers(), files, file_done, nullptr,
                                               batch_progress, batch_stopping);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->connections = 0;
                shared_->conn_report.clear();
            }
            using End = HelperCopy::End;
            const std::string failed_path =
                batch.failed >= 0 && static_cast<std::size_t>(batch.failed) < todo.size()
                    ? plan.files[todo[static_cast<std::size_t>(batch.failed)]].path
                    : std::string();
            std::vector<std::size_t> left;
            for (std::size_t k = 0; k < todo.size(); ++k)
                if (!got[k])
                    left.push_back(todo[k]);
            todo.swap(left);
            if (batch.end != End::unavailable && batch.end != End::unsupported)
                helper_used_ = true;
            switch (batch.end)
            {
            case End::ok:
                if (todo.empty())
                    return Result::ok;
                break; // it said done with files missing: again
            case End::stopped:
                return Result::stopped;
            case End::unsupported:
                return Result::fallback;
            case End::unavailable:
                if (!helper_used_)
                {
                    helper_unavailable_ = true; // in-app from here on
                    return Result::fallback;
                }
                break; // it ran before: it may come back, so try again
            case End::refused:
            {
                char text[64];
                std::snprintf(text, sizeof(text), " (HTTP %d)", batch.status);
                *why = "The NAS refused " + failed_path +
                       (batch.status != 0 ? std::string(text) : " (" + batch.why + ")");
                return Result::failed;
            }
            case End::file:
                *why = "Couldn't create " + failed_path + " on the drive (" + batch.why + ")";
                return Result::failed;
            case End::write:
                *why = "Couldn't write to the drive (" + batch.why + ")";
                return Result::failed;
            case End::changed:
                *why = "A file changed on the NAS since the library was read: refresh it and try again";
                return Result::failed;
            case End::lost:
            case End::network:
                break;
            }
            if (stopping())
                return Result::stopped;
            if (got_count > 0)
                failures = 0; // it got somewhere: the count starts again
            if (++failures > kRetries)
            {
                *why = batch.end == End::network ? "Lost the connection to the NAS"
                                                 : "The copy helper stopped working";
                return Result::failed;
            }
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail = batch.why.empty()
                                      ? std::string("Connection lost, trying again")
                                      : "Connection lost (" + batch.why + "), trying again";
                shared_->rate = 0.0;
            }
            for (int waited = 0; waited < failures * 2000 && !stopping(); waited += 100)
                sleep_ms(100);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail.clear();
            }
        }
        return Result::ok;
    }

    // ---- a folder game as one exFAT image, through the copy helper payload ----

    static std::vector<ImageFile> image_files(const CopyPlan &plan)
    {
        std::vector<ImageFile> files;
        files.reserve(plan.files.size());
        for (const CopyFile &file : plan.files)
            files.push_back({file.path, file.size});
        return files;
    }

    // FNV-1a over the pieces: what the image's bytes will be, but for the game's.
    static std::uint64_t layout_of(const ExfatImage &image)
    {
        std::uint64_t hash = 14695981039346656037ull;
        const auto mix = [&](const void *data, std::size_t length)
        {
            const unsigned char *bytes = static_cast<const unsigned char *>(data);
            for (std::size_t i = 0; i < length; ++i)
                hash = (hash ^ bytes[i]) * 1099511628211ull;
        };
        for (const ImagePiece &piece : image.pieces)
        {
            const char kind = static_cast<char>(piece.kind);
            mix(&kind, 1);
            mix(&piece.length, sizeof(piece.length));
            mix(&piece.file, sizeof(piece.file));
            mix(piece.data.data(), piece.data.size());
        }
        return hash == 0 ? 1 : hash;
    }

    // The image into staging_, front to back, carrying on from what's there.
    Result copy_image(const CopyPlan &plan, const ExfatImage &image, std::string *why)
    {
        if (!copy_helper_enabled())
        {
            *why = "it needs the copy helper, which isn't available";
            return Result::fallback;
        }
        // The image in the helper's words; where each file ends in it (so the
        // file count follows the copy), and how many are empty (no piece).
        std::string pieces;
        std::vector<std::uint64_t> ends;
        std::uint64_t at = 0;
        for (const ImagePiece &piece : image.pieces)
        {
            at += piece.length;
            switch (piece.kind)
            {
            case ImagePiece::Kind::bytes:
                pieces += "D " + std::to_string(piece.length) + "\n" + piece.data;
                break;
            case ImagePiece::Kind::zeros:
                pieces += "Z " + std::to_string(piece.length) + "\n";
                break;
            case ImagePiece::Kind::file:
            {
                const std::string remote = plan.source + plan.files[piece.file].path;
                pieces += "F " + std::to_string(piece.length) + " " +
                          (smb() ? smb_location(remote).path : percent_encode_path(remote)) + "\n";
                ends.push_back(at);
                break;
            }
            }
        }
        const int empty = static_cast<int>(plan.files.size() - ends.size());
        // Over WebDAV, the header lines every GET carries, kept open between files.
        std::string headers;
        if (!smb())
        {
            const std::string request = build_request(connection_, "GET", "/", "", "");
            const std::size_t first = request.find("\r\n") + 2;
            headers = request.substr(first, request.size() - first - 2); // not the blank line
            const std::size_t close_at = headers.find("Connection: close\r\n");
            if (close_at != std::string::npos)
                headers.replace(close_at, 17, "Connection: keep-alive");
        }
        const SmbSignIn smb_sign_in = smb() ? sign_in() : SmbSignIn{};
        // Big files come in a few parts at a time; small ones several at once.
        const int workers = std::max(helper_streams(), std::min(batch_workers(), 8));
        int failures = 0;
        while (true)
        {
            if (stopping())
                return Result::stopped;
            std::uint64_t first = ~0ull, last = 0;
            window_start_ = now_ms();
            window_bytes_ = 0;
            const auto image_progress =
                [&](std::uint64_t position, std::uint64_t write_ms, std::uint64_t elapsed_ms, int connections)
                {
                    if (first == ~0ull)
                        first = last = position;
                    measure(position, position > last ? static_cast<std::size_t>(position - last) : 0);
                    last = position;
                    const int files_in =
                        empty + static_cast<int>(std::upper_bound(ends.begin(), ends.end(), position) - ends.begin());
                    char report[64];
                    std::snprintf(report, sizeof(report), "helper image x%d  write %d%%", connections,
                                  elapsed_ms > 0 ? static_cast<int>(write_ms * 100 / elapsed_ms) : 0);
                    std::lock_guard<std::mutex> hold(shared_->lock);
                    shared_->files_done = files_in;
                    shared_->connections = connections > 0 ? connections : 1;
                    shared_->conn_report = report;
                };
            const auto image_stopping = [this] { return stopping(); };
            const HelperCopy copy = copy_image_with_helper(
                connection_.host, connection_.effective_port(), image.size, staging_, headers,
                smb() ? &smb_sign_in : nullptr, pieces, workers, image_progress, image_stopping);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->connections = 0;
                shared_->conn_report.clear();
            }
            using End = HelperCopy::End;
            if (copy.end != End::unavailable)
                helper_used_ = true;
            switch (copy.end)
            {
            case End::ok:
                return Result::ok;
            case End::stopped:
                return Result::stopped;
            case End::unavailable:
                if (!helper_used_)
                {
                    *why = "it needs the copy helper, which didn't start";
                    return Result::fallback;
                }
                break; // it ran before: it may come back, so try again
            case End::unsupported:
                if (copy.why.find("copy request") != std::string::npos)
                    *why = "The copy helper running on the PS5 is from an older NASBridge. It "
                           "closes after 2 minutes unused; then Resume";
                else
                {
                    *why = "this NAS won't send part of a file (" + copy.why + ")";
                    return Result::fallback;
                }
                return Result::failed;
            case End::refused:
            {
                char text[96];
                std::snprintf(text, sizeof(text), "The NAS refused a file (HTTP %d)", copy.status);
                *why = copy.status != 0 ? std::string(text) + " " + copy.why.substr(copy.why.find(' ') + 1)
                                        : "The NAS refused a file (" + copy.why + ")";
                return Result::failed;
            }
            case End::file:
                *why = "Couldn't create the image on the drive (" + copy.why + ")";
                return Result::failed;
            case End::write:
                *why = "Couldn't write to the drive (" + copy.why + ")";
                return Result::failed;
            case End::changed:
                *why = "A file changed on the NAS since the library was read: refresh it and try again";
                return Result::failed;
            case End::lost:
            case End::network:
                break;
            }
            if (stopping())
                return Result::stopped;
            if (first != ~0ull && last > first + kRefetch)
                failures = 0; // it got somewhere: the count starts again
            if (++failures > kRetries)
            {
                *why = copy.end == End::network ? "Lost the connection to the NAS"
                                                : "The copy helper stopped working";
                return Result::failed;
            }
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail = copy.why.empty()
                                      ? std::string("Connection lost, trying again")
                                      : "Connection lost (" + copy.why + "), trying again";
                shared_->rate = 0.0;
            }
            for (int waited = 0; waited < failures * 2000 && !stopping(); waited += 100)
                sleep_ms(100);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail.clear();
            }
        }
    }

    // Reads the copied image back: every folder and file where the layout
    // put it, at its size. One that doesn't is deleted, so Resume starts it
    // again rather than adding a broken game.
    bool check_image(const CopyPlan &plan, std::string *why)
    {
        const int fd = ::open(staging_.c_str(), O_RDONLY);
        if (fd < 0)
        {
            *why = errno_text("Couldn't open the copied image", errno);
            return false;
        }
        const ExfatReader::ReadAt read = [fd](std::uint64_t offset, std::size_t length, std::string *out)
        {
            out->assign(length, '\0');
            std::size_t got = 0;
            while (got < length)
            {
                const ssize_t n = ::pread(fd, &(*out)[got], length - got, static_cast<off_t>(offset + got));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    return false;
                got += static_cast<std::size_t>(n);
            }
            return true;
        };
        std::string problem;
        const bool ok = check_exfat_image(read, plan.folders, image_files(plan), &problem);
        ::close(fd);
        if (ok)
            return true;
        ::unlink(staging_.c_str());
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            if (Item *item = shared_->item(entry_))
                item->done = 0;
            shared_->save();
        }
        *why = "The copied image didn't read back as the game (" + problem +
               "), so it was deleted. Resume copies it again";
        return false;
    }

    // ---- a file, through the copy helper payload ----

    Result copy_via_helper(const std::string &remote, const std::string &local, std::uint64_t size,
                           std::uint64_t before, std::string *why)
    {
        // Pieces of this file from an in-app copy (an older NASBridge, or
        // without the helper): the helper writes files front to back, so it
        // starts this one again.
        const std::string part = local + kPartSuffix;
        if (exists(part))
        {
            ::unlink(part.c_str());
            forget_part(entry_);
        }
        const std::string request = smb() ? "" : build_request(connection_, "GET", remote, "", "");
        int failures = 0;
        while (true)
        {
            if (stopping())
                return Result::stopped;
            std::uint64_t first = ~0ull, last = 0;
            window_start_ = now_ms();
            window_bytes_ = 0;
            if (helper_streams_ == 0)
                helper_streams_ = helper_streams();
            const auto copy_progress =
                [&](std::uint64_t at, std::uint64_t write_ms, std::uint64_t elapsed_ms, int connections)
                {
                    if (first == ~0ull)
                        first = last = at;
                    measure(before + at, at > last ? static_cast<std::size_t>(at - last) : 0);
                    last = at;
                    // The speed tile's connection count, and the diagnostic line:
                    // "helper" says the payload is copying; write % as before.
                    // "helper x2" while two connections are bringing data in.
                    char report[64];
                    char streams[8] = "";
                    if (connections >= 2)
                        std::snprintf(streams, sizeof(streams), " x%d", connections);
                    std::snprintf(report, sizeof(report), "helper%s  write %d%%", streams,
                                  elapsed_ms > 0 ? static_cast<int>(write_ms * 100 / elapsed_ms) : 0);
                    std::lock_guard<std::mutex> hold(shared_->lock);
                    shared_->connections = connections > 0 ? connections : 1;
                    shared_->conn_report = report;
                };
            const auto copy_stopping = [this] { return stopping(); };
            const HelperCopy copy =
                smb() ? copy_smb_with_helper(connection_.host, connection_.effective_port(), size,
                                             local, sign_in(), smb_location(remote).path,
                                             helper_streams_, copy_progress, copy_stopping)
                      : copy_with_helper(connection_.host, connection_.effective_port(), size,
                                         local, request, helper_streams_, copy_progress,
                                         copy_stopping);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->connections = 0;
                shared_->conn_report.clear();
            }
            using End = HelperCopy::End;
            if (copy.end != End::unavailable)
                helper_used_ = true;
            switch (copy.end)
            {
            case End::ok:
                return Result::ok;
            case End::stopped:
                return Result::stopped;
            case End::unavailable:
                if (!helper_used_)
                {
                    helper_unavailable_ = true; // in-app from here on
                    return Result::fallback;
                }
                break; // it ran before: it may come back, so try again
            case End::unsupported:
                return Result::fallback; // this file in-app; the helper stays on
            case End::refused:
            {
                char text[96];
                std::snprintf(text, sizeof(text), "The NAS refused a file (HTTP %d)", copy.status);
                *why = copy.status != 0 ? std::string(text) : "The NAS refused a file (" + copy.why + ")";
                return Result::failed;
            }
            case End::file:
                *why = "Couldn't create a file on the drive (" + copy.why + ")";
                return Result::failed;
            case End::write:
                *why = "Couldn't write to the drive (" + copy.why + ")";
                return Result::failed;
            case End::changed:
                *why = "A file changed on the NAS since the library was read: refresh it and try again";
                return Result::failed;
            case End::lost:
            case End::network:
                break;
            }
            if (stopping())
                return Result::stopped;
            // Two connections that fail before anything arrives: one from here
            // on for this game (one has always worked; this keeps a problem
            // with the second from stopping a copy).
            const bool progressed = first != ~0ull && last > first;
            if (!progressed && helper_streams_ > 1)
                helper_streams_ = 1;
            // Cut off: try again, from where it got to. Progress resets the count.
            if (first != ~0ull && last > first + kRefetch)
                failures = 0;
            if (++failures > kRetries)
            {
                if (copy.end == End::network)
                    *why = copy.status != 0 ? "The NAS keeps dropping the connection"
                                            : "Lost the connection to the NAS";
                else
                    *why = "The copy helper stopped working";
                return Result::failed;
            }
            {
                // The helper's own words, so a loop like this can be told apart
                // ("the NAS went quiet", "the copy helper stopped answering").
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail = copy.why.empty()
                                      ? std::string("Connection lost, trying again")
                                      : "Connection lost (" + copy.why + "), trying again";
                shared_->rate = 0.0;
            }
            for (int waited = 0; waited < failures * 2000 && !stopping(); waited += 100)
                sleep_ms(100);
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail.clear();
            }
        }
    }

    // ---- a big file, a piece per connection ----

    struct Pieces
    {
        std::mutex lock;
        std::vector<char> done;
        std::vector<char> claimed;
        std::uint64_t finished = 0;            // bytes in done pieces
        std::atomic<std::uint64_t> flowing{0}; // bytes of pieces still coming in
        std::atomic<bool> halt{false};         // stop every connection (an error, no ranges)
        bool no_ranges = false;
        // Pieces have come in for this file before (recorded, or kept from a
        // file begun in one go), or one has in this run: the NAS does send
        // pieces, so an odd answer later is a hiccup to retry, never a reason
        // to start the file again in one go.
        std::atomic<bool> ranges_work{false};
        // Connections at work: delivering a piece, or moving on to the next
        // one after a piece came in whole. A failed piece takes it off.
        std::atomic<int> active{0};
        std::mutex record_lock; // one write of the pieces record at a time
        std::atomic<std::uint64_t> last_progress{0}; // now_ms() when data last arrived
        // Per-connection diagnostics, one slot each, for the queue panel.
        struct Slot
        {
            std::atomic<char> state{'.'};        // . idle  c connect  t transfer  r retry  x refused
            std::atomic<int> status{0};          // last HTTP status
            std::atomic<std::uint64_t> bytes{0}; // bytes this connection delivered
        };
        std::atomic<int> next_slot{0};
        Slot slot[kStreams];
        std::atomic<std::uint64_t> last_record_ms{0}; // throttles the record's fsync
        // How much connection-time goes into pwrite, to tell a write
        // bottleneck (the drive) from a read one (the NAS).
        std::atomic<std::uint64_t> write_ns{0};
        std::uint64_t start_ms = 0;
        int streams = 1; // connections this copy uses (g_copy_streams, clamped), set at setup
        std::uint64_t seq_pos = ~0ull; // one-stream: the fd's write position, to write sequentially
        // The file is fsync'd every kSyncMs or kSyncBytes, whichever comes first.
        std::atomic<std::uint64_t> unsynced{0};     // bytes written since the last fsync
        std::atomic<std::uint64_t> last_sync_ms{0}; // when it last ran (one connection runs it)
        std::string error;
        int fd = -1;
        std::uint64_t size = 0;
        std::string remote;
        std::string path; // relative, for the record
        std::uint64_t before = 0;
    };

    struct Stream
    {
        Copier *copier;
        Pieces *pieces;
    };

    static void *stream_main(void *argument)
    {
        Stream *stream = static_cast<Stream *>(argument);
        keep_off_cpu(stream->copier->shared_->ui_cpu);
        stream->copier->stream(*stream->pieces);
        return nullptr;
    }

    // One connection: takes pieces that nobody has until none are left.
    void stream(Pieces &pieces)
    {
        std::vector<char> buffer;
        buffer.reserve(kPieceBuffer + 64 * 1024);
        int failures = 0; // in a row, on this connection
        bool counted = false; // this connection is in pieces.active
        const int sidx = pieces.next_slot.fetch_add(1);
        Pieces::Slot *rep = sidx < kStreams ? &pieces.slot[sidx] : nullptr;
        struct Uncount
        {
            Pieces &pieces;
            bool &counted;
            ~Uncount()
            {
                if (counted)
                    pieces.active.fetch_sub(1);
            }
        } uncount{pieces, counted};
        while (!stopping() && !pieces.halt.load())
        {
            // Each connection works a contiguous quarter of the file first, so
            // its writes to the drive are sequential, not scattered 128 MB
            // apart (which collapses write speed, badly on exFAT). Only when
            // its own region is done does it help finish the others'.
            std::size_t piece = 0;
            {
                std::lock_guard<std::mutex> hold(pieces.lock);
                const std::size_t n = pieces.done.size();
                const std::size_t per = (n + pieces.streams - 1) / pieces.streams;
                const std::size_t rstart =
                    std::min(n, static_cast<std::size_t>(std::max(sidx, 0)) * per);
                const std::size_t rend = std::min(n, rstart + per);
                piece = n;
                for (std::size_t i = rstart; i < rend; ++i)
                    if (!pieces.done[i] && !pieces.claimed[i])
                    {
                        piece = i;
                        break;
                    }
                if (piece == n) // own region done: take the next free piece anywhere
                    for (std::size_t i = 0; i < n; ++i)
                        if (!pieces.done[i] && !pieces.claimed[i])
                        {
                            piece = i;
                            break;
                        }
                if (piece == n)
                    return; // all taken: the others finish theirs
                pieces.claimed[piece] = 1;
            }
            const std::uint64_t start = static_cast<std::uint64_t>(piece) * kPiece;
            const std::uint64_t length = std::min(kPiece, pieces.size - start);
            std::uint64_t written = 0;
            bool write_failed = false;
            int write_error = 0;
            buffer.clear();
            const auto flush_piece = [&]() -> bool
            {
                const std::uint64_t t0 = now_ns();
                // One connection writes the file strictly in order, so it writes
                // sequentially with write() (seeking only when a refetch rewinds
                // it) rather than pwrite at an absolute offset. On exFAT, each
                // pwrite resolves its offset by walking the allocation chain from
                // the start, turning a big copy into O(n^2) and collapsing write
                // speed as the file grows; sequential write() advances cluster by
                // cluster. Several connections must pwrite (scattered offsets).
                const bool sequential = pieces.streams == 1;
                const std::uint64_t offset = start + written;
                if (sequential && pieces.seq_pos != offset)
                {
                    if (::lseek(pieces.fd, static_cast<off_t>(offset), SEEK_SET) < 0)
                    {
                        write_error = errno;
                        return false;
                    }
                    pieces.seq_pos = offset;
                }
                std::size_t at = 0;
                while (at < buffer.size())
                {
                    const ssize_t n =
                        sequential
                            ? ::write(pieces.fd, buffer.data() + at, buffer.size() - at)
                            : ::pwrite(pieces.fd, buffer.data() + at, buffer.size() - at,
                                       static_cast<off_t>(offset + at));
                    if (n <= 0)
                    {
                        if (n < 0 && errno == EINTR)
                            continue;
                        write_error = n == 0 ? EIO : errno;
                        return false;
                    }
                    at += static_cast<std::size_t>(n);
                    if (sequential)
                        pieces.seq_pos += static_cast<std::uint64_t>(n);
                }
                // Flush to the drive every kSyncMs or kSyncBytes. The PS5
                // (FreeBSD UFS) lets unflushed writes pile up until it hits its
                // dirty-buffer limit, then throttles every writer hard: a copy
                // that starts at 30+ MB/s falls to 2-3 MB/s a few GB in.
                // Flushing little and often keeps it streaming (ps5upload does
                // the same, for the same reason).
                {
                    const std::uint64_t pending =
                        pieces.unsynced.fetch_add(buffer.size()) + buffer.size();
                    const std::uint64_t now = now_ms();
                    std::uint64_t was = pieces.last_sync_ms.load();
                    if ((pending >= kSyncBytes || now - was >= kSyncMs) &&
                        pieces.last_sync_ms.compare_exchange_strong(was, now))
                    {
                        pieces.unsynced.store(0);
                        if (::fsync(pieces.fd) != 0 && errno != EINVAL && errno != ENOTSUP)
                        {
                            write_error = errno;
                            pieces.write_ns.fetch_add(now_ns() - t0);
                            return false;
                        }
                    }
                }
                pieces.write_ns.fetch_add(now_ns() - t0);
                written += buffer.size();
                buffer.clear();
                return true;
            };
            std::uint64_t received = 0;
            bool odd = false;        // the NAS answered something other than the piece
            if (rep)
                rep->state.store('c');
            const std::string range = "Range: bytes=" + std::to_string(start) + "-" +
                                      std::to_string(start + length - 1) + "\r\n";
            const HttpReply reply = http_stream(
                connection_, "GET", pieces.remote, range, "",
                [&](const char *data, std::size_t size)
                {
                    if (stopping() || pieces.halt.load())
                        return false;
                    if (received + size > length)
                        size = static_cast<std::size_t>(length - received);
                    buffer.insert(buffer.end(), data, data + size);
                    received += size;
                    if (rep)
                        rep->bytes.fetch_add(size);
                    pieces.flowing.fetch_add(size);
                    pieces.last_progress.store(now_ms());
                    if (buffer.size() >= kPieceBuffer && !flush_piece())
                    {
                        write_failed = true;
                        return false;
                    }
                    measure_pieces(pieces, size);
                    return received < length;
                },
                kReplySeconds,
                [&](const HttpReply &header)
                {
                    // The piece asked for, or the NAS doesn't do pieces.
                    const std::string expected = "bytes " + std::to_string(start) + "-";
                    if (rep)
                        rep->status.store(header.status);
                    if (header.status == 206 &&
                        header_value(header.header, "Content-Range").compare(0, expected.size(),
                                                                            expected) == 0)
                    {
                        pieces.ranges_work.store(true);
                        if (rep)
                            rep->state.store('t');
                        if (!counted)
                            pieces.active.fetch_add(1);
                        counted = true;
                        return true;
                    }
                    if ((header.status == 200 || header.status == 206) &&
                        !pieces.ranges_work.load())
                    {
                        // The very first answers: this NAS sends whole files only.
                        std::lock_guard<std::mutex> hold(pieces.lock);
                        pieces.no_ranges = true;
                        pieces.halt.store(true);
                    }
                    odd = true; // retried below, like a dropped connection
                    return false;
                },
                [&] { return stopping() || pieces.halt.load(); });
            if (!write_failed && !buffer.empty() && !flush_piece())
                write_failed = true;
            pieces.flowing.fetch_sub(received);
            if (write_failed)
            {
                std::lock_guard<std::mutex> hold(pieces.lock);
                pieces.claimed[piece] = 0;
                if (pieces.error.empty())
                    pieces.error = errno_text("Couldn't write to the drive", write_error);
                pieces.halt.store(true);
                return;
            }
            if (written == length)
            {
                // In: still counted as at work while it moves on to the next.
                {
                    std::lock_guard<std::mutex> hold(pieces.lock);
                    pieces.done[piece] = 1;
                    pieces.claimed[piece] = 0;
                    pieces.finished += length;
                }
                // Record progress for resume, but not after every piece: the
                // record is fsync'd each time, and doing that per 32 MB piece
                // throttles the whole copy more and more as the file grows. A
                // few seconds' staleness only costs re-fetching a handful of
                // pieces after a crash. The file's completion removes the
                // record anyway, and a final save on exit keeps pause accurate.
                const std::uint64_t now = now_ms();
                std::uint64_t prev = pieces.last_record_ms.load();
                if (now - prev >= 3000 &&
                    pieces.last_record_ms.compare_exchange_strong(prev, now))
                {
                    std::lock_guard<std::mutex> writing(pieces.record_lock);
                    std::vector<char> done;
                    {
                        std::lock_guard<std::mutex> hold(pieces.lock);
                        done = pieces.done;
                    }
                    save_part(entry_, pieces.path, pieces.size, done);
                }
                failures = 0;
                continue;
            }
            if (counted)
            {
                // Not at work until a piece comes in again.
                pieces.active.fetch_sub(1);
                counted = false;
            }
            {
                std::lock_guard<std::mutex> hold(pieces.lock);
                pieces.claimed[piece] = 0; // someone (maybe this one) fetches it again
            }
            if (stopping() || pieces.halt.load())
                return;
            // A plain no (missing, forbidden) ends the copy; "busy" and
            // "try later" answers are waited out like a dropped connection.
            const bool final_no = reply.status >= 400 && reply.status < 500 &&
                                  reply.status != 408 && reply.status != 429;
            if (!odd && reply.connected && final_no)
            {
                if (rep)
                    rep->state.store('x');
                std::lock_guard<std::mutex> hold(pieces.lock);
                if (pieces.error.empty())
                {
                    char text[96];
                    std::snprintf(text, sizeof(text), "The NAS refused a file (HTTP %d)",
                                  reply.status);
                    pieces.error = text;
                }
                pieces.halt.store(true);
                return;
            }
            if (rep)
                rep->state.store('r');
            // Cut off, refused, or an odd answer. This connection tries again,
            // waiting longer each time (up to half a minute), for as long as
            // the file is still arriving; a NAS that allows fewer connections
            // just keeps it waiting. Only when nothing at all has arrived for
            // two minutes does it give up.
            ++failures;
            if (failures > kRetries &&
                now_ms() - pieces.last_progress.load() > kStalledSeconds * 1000ull)
                return;
            if (pieces.active.load() == 0)
            {
                // Only said when no other connection is bringing data in.
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail = "Connection lost, trying again";
                shared_->rate = 0.0;
            }
            const int wait_ms = std::min(failures * 2, kRejoinSeconds) * 1000;
            for (int waited = 0; waited < wait_ms && !stopping() && !pieces.halt.load();
                 waited += 100)
            {
                // Every piece in while this one waited? Then it isn't needed.
                bool all_in;
                {
                    std::lock_guard<std::mutex> hold(pieces.lock);
                    all_in = std::find(pieces.done.begin(), pieces.done.end(), 0) ==
                             pieces.done.end();
                }
                if (all_in)
                    break;
                sleep_ms(100);
            }
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->detail.clear();
            }
        }
    }

    void measure_pieces(Pieces &pieces, std::size_t length)
    {
        std::uint64_t finished;
        {
            std::lock_guard<std::mutex> hold(pieces.lock);
            finished = pieces.finished;
        }
        // A per-connection line for the queue panel, rebuilt a few times a
        // second: "1:t206 420M  2:r503 12M  3:t206 410M  4:.0 0M".
        const std::uint64_t now = now_ms();
        std::uint64_t was = last_report_ms_.load();
        if (now - was >= 400 && last_report_ms_.compare_exchange_strong(was, now))
        {
            std::string report;
            for (int i = 0; i < pieces.streams; ++i)
            {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%d:%c%d %lluM  ", i + 1,
                              pieces.slot[i].state.load(), pieces.slot[i].status.load(),
                              static_cast<unsigned long long>(pieces.slot[i].bytes.load() >> 20));
                report += buf;
            }
            // Share of connection-time spent in pwrite: high means the drive
            // is the bottleneck, low means the NAS is.
            const std::uint64_t elapsed_ms = now - pieces.start_ms;
            if (elapsed_ms > 0)
            {
                const std::uint64_t busy = pieces.write_ns.load() / 1000000ull;
                const int pct = static_cast<int>(
                    busy * 100ull / (elapsed_ms * static_cast<std::uint64_t>(pieces.streams)));
                char buf[24];
                std::snprintf(buf, sizeof(buf), "write %d%%", pct);
                report += buf;
            }
            std::lock_guard<std::mutex> hold(shared_->lock);
            shared_->connections = pieces.active.load();
            shared_->conn_report = report;
        }
        else
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            shared_->connections = pieces.active.load();
        }
        std::lock_guard<std::mutex> hold(measure_lock_);
        measure(pieces.before + finished + pieces.flowing.load(), length);
    }

    Result copy_pieces(const std::string &remote, const std::string &local, std::uint64_t size,
                       std::uint64_t before, const std::string &path, std::string *why)
    {
        const std::string part = local + kPartSuffix;
        // Already in under its final name? Or begun in one go, from the start
        // (by an older NASBridge, or by this one's fallback): what's there
        // becomes the first pieces, and the rest come in pieces.
        std::uint64_t adopted = 0; // bytes at the start known to be in
        {
            const int existing = ::open(local.c_str(), O_RDONLY);
            if (existing >= 0)
            {
                const off_t end = ::lseek(existing, 0, SEEK_END);
                ::close(existing);
                if (end == static_cast<off_t>(size))
                    return Result::ok;
                // Its last few megabytes may not have reached the disk before
                // it stopped: those are fetched again.
                const std::uint64_t have = end > 0 ? static_cast<std::uint64_t>(end) : 0;
                adopted = have > kRefetch ? have - kRefetch : 0;
                if (adopted < kPiece || ::rename(local.c_str(), part.c_str()) != 0)
                    return Result::fallback; // too little to bother, or it can't be moved
                forget_part(entry_);
            }
        }
        Pieces pieces;
        pieces.size = size;
        pieces.remote = remote;
        pieces.path = path;
        pieces.before = before;
        pieces.done.assign(static_cast<std::size_t>((size + kPiece - 1) / kPiece), 0);
        pieces.claimed.assign(pieces.done.size(), 0);
        pieces.fd = ::open(part.c_str(), O_RDWR | O_CREAT, 0777);
        if (pieces.fd < 0)
        {
            *why = errno_text("Couldn't create a file on the drive", errno);
            return Result::failed;
        }
        {
            const int want = g_copy_streams.load();
            pieces.streams = want < 1 ? 1 : (want > kStreams ? kStreams : want);
        }
        // One connection writes front to back, so its file grows as it's
        // written instead of being made full size up front with ftruncate,
        // which on the PS5 leaves a sparse file (no blocks behind it): filling
        // a sparse file is the case ps5upload measured collapsing from 60 to
        // 2-3 MB/s under dirty-buffer throttling. A growing file, flushed every
        // kSyncMs, keeps its speed. Several connections write at scattered
        // offsets and still need the full size first.
        const bool grow = pieces.streams == 1;
        // Carry on from the pieces recorded, or start the file (at its full
        // size, unless it grows).
        if (adopted > 0)
        {
            // A file begun in one go: every whole piece before its end is in.
            if (!grow && ::ftruncate(pieces.fd, static_cast<off_t>(size)) != 0)
            {
                *why = errno_text("Couldn't make room for a file", errno);
                ::close(pieces.fd);
                return Result::failed;
            }
            for (std::size_t i = 0; i < pieces.done.size(); ++i)
            {
                const std::uint64_t end =
                    std::min(size, static_cast<std::uint64_t>(i + 1) * kPiece);
                pieces.done[i] = end <= adopted ? 1 : 0;
            }
            save_part(entry_, path, size, pieces.done);
        }
        bool resumed = adopted > 0;
        if (!resumed && load_part(entry_, path, size, &pieces.done))
        {
            const off_t end = ::lseek(pieces.fd, 0, SEEK_END);
            const std::uint64_t have = end > 0 ? static_cast<std::uint64_t>(end) : 0;
            if (grow)
            {
                // Only pieces wholly inside the file as it stands are in (a
                // full-size file from a several-connection copy works too).
                for (std::size_t i = 0; i < pieces.done.size(); ++i)
                    if (pieces.done[i] &&
                        std::min(size, static_cast<std::uint64_t>(i + 1) * kPiece) > have)
                        pieces.done[i] = 0;
                resumed = true;
            }
            else
            {
                resumed = have == size;
            }
        }
        if (!resumed)
        {
            std::fill(pieces.done.begin(), pieces.done.end(), 0);
            if (::ftruncate(pieces.fd, 0) != 0 ||
                (!grow && ::ftruncate(pieces.fd, static_cast<off_t>(size)) != 0))
            {
                *why = errno_text("Couldn't make room for a file", errno);
                ::close(pieces.fd);
                return Result::failed;
            }
            save_part(entry_, path, size, pieces.done);
        }
        for (std::size_t i = 0; i < pieces.done.size(); ++i)
            if (pieces.done[i])
                pieces.finished += std::min(kPiece, size - static_cast<std::uint64_t>(i) * kPiece);
        // Pieces recorded for this file: the NAS has sent pieces before. (A
        // file begun in one go proves nothing either way.)
        pieces.ranges_work.store(adopted == 0 && pieces.finished > 0);
        pieces.last_progress.store(now_ms());
        pieces.start_ms = now_ms();
        pieces.last_sync_ms.store(now_ms());
        window_start_ = now_ms();
        window_bytes_ = 0;

        // The connections: this thread is one of them.
        Stream stream_args{this, &pieces};
        pthread_t threads[kStreams - 1]; // sized for the most; only pieces.streams-1 are spawned
        int started = 0;
        for (int i = 0; i < pieces.streams - 1; ++i)
        {
            pthread_attr_t attributes;
            pthread_attr_init(&attributes);
            pthread_attr_setstacksize(&attributes, 1024 * 1024);
            if (pthread_create(&threads[started], &attributes, &Copier::stream_main,
                               &stream_args) == 0)
                ++started;
            pthread_attr_destroy(&attributes);
        }
        stream(pieces);
        for (int i = 0; i < started; ++i)
            pthread_join(threads[i], nullptr);
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            shared_->connections = 0;
            shared_->conn_report.clear();
        }
        // The per-piece saves were throttled, so bring the record up to date
        // now (a pause resumes from here; completion removes it below).
        if (!pieces.no_ranges)
            save_part(entry_, path, size, pieces.done);

        if (pieces.no_ranges)
        {
            // Back to one go. A file begun in one go goes back as it was, so
            // what it had is kept.
            if (adopted > 0 && ::ftruncate(pieces.fd, static_cast<off_t>(adopted)) == 0)
            {
                ::close(pieces.fd);
                ::rename(part.c_str(), local.c_str());
            }
            else
            {
                ::close(pieces.fd);
                ::unlink(part.c_str());
            }
            forget_part(entry_);
            return Result::fallback;
        }
        bool complete = true;
        for (const char piece : pieces.done)
            complete = complete && piece;
        if (!pieces.error.empty())
        {
            *why = pieces.error;
            ::close(pieces.fd);
            return Result::failed;
        }
        if (stopping())
        {
            ::close(pieces.fd);
            return Result::stopped; // the record says which pieces are in
        }
        if (!complete)
        {
            *why = "Lost the connection to the NAS";
            ::close(pieces.fd);
            return Result::failed;
        }
        if (grow && ::lseek(pieces.fd, 0, SEEK_END) != static_cast<off_t>(size))
        {
            // Every piece is in, so a growing file must have reached its size.
            *why = "A copied file came out the wrong size";
            ::close(pieces.fd);
            return Result::failed;
        }
        if (::fsync(pieces.fd) != 0)
        {
            *why = errno_text("Couldn't finish writing a file", errno);
            ::close(pieces.fd);
            return Result::failed;
        }
        ::close(pieces.fd);
        if (::rename(part.c_str(), local.c_str()) != 0)
        {
            *why = errno_text("Couldn't finish a file", errno);
            return Result::failed;
        }
        forget_part(entry_);
        return Result::ok;
    }

    // Where a file carries on from, with fd there: how far it got (on the
    // first pass, an earlier run's work), less its last part, which may not
    // have reached the disk before a crash or power cut. *whole: it's all in.
    bool resume_at(int fd, std::uint64_t size, std::uint64_t *offset, bool *whole, std::string *why)
    {
        *whole = false;
        const off_t end = ::lseek(fd, 0, SEEK_END);
        if (end < 0)
        {
            *why = errno_text("Couldn't read a file on the drive", errno);
            return false;
        }
        std::uint64_t have = static_cast<std::uint64_t>(end);
        if (have == size)
        {
            *whole = true;
            return true;
        }
        if (have > size)
        {
            // Bigger than on the NAS (it changed): start the file again.
            if (::ftruncate(fd, 0) != 0)
            {
                *why = errno_text("Couldn't restart a file", errno);
                return false;
            }
            have = 0;
        }
        *offset = have > kRefetch ? have - kRefetch : 0;
        *offset -= *offset % (1024 * 1024);
        if (::lseek(fd, static_cast<off_t>(*offset), SEEK_SET) < 0)
        {
            *why = errno_text("Couldn't seek in a file", errno);
            return false;
        }
        return true;
    }

    // A connection was lost mid-file: waits a little longer each time, and
    // says so. False when it has happened too often.
    bool retry_after(int *failures, bool progressed, const std::string &gave_up, std::string *why)
    {
        if (progressed)
            *failures = 0;
        if (++*failures > kRetries)
        {
            *why = gave_up;
            return false;
        }
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            shared_->detail = "Connection lost, trying again";
            shared_->rate = 0.0;
        }
        for (int waited = 0; waited < *failures * 2000 && !stopping(); waited += 100)
            sleep_ms(100);
        {
            std::lock_guard<std::mutex> hold(shared_->lock);
            shared_->detail.clear();
        }
        return true;
    }

    Result fill(int fd, const std::string &remote, std::uint64_t size, std::uint64_t before,
                std::string *why)
    {
        int failures = 0;
        while (true)
        {
            std::uint64_t offset = 0;
            bool whole = false;
            if (!resume_at(fd, size, &offset, &whole, why))
                return Result::failed;
            if (whole)
                return Result::ok;
            if (stopping())
                return Result::stopped;

            std::uint64_t position = offset;
            bool write_failed = false;
            int write_error = 0;
            int status = 0;
            const std::string range =
                offset > 0 ? "Range: bytes=" + std::to_string(offset) + "-\r\n" : "";
            buffer_.clear();
            window_start_ = now_ms();
            window_bytes_ = 0;
            bool counted = false;
            const HttpReply reply = http_stream(
                connection_, "GET", remote, range, "",
                [&](const char *data, std::size_t length)
                {
                    if (stopping())
                        return false;
                    buffer_.insert(buffer_.end(), data, data + length);
                    position += length;
                    if (buffer_.size() >= kWriteBlock && !flush(fd))
                    {
                        write_failed = true;
                        write_error = errno;
                        return false;
                    }
                    if (!counted)
                    {
                        // One connection, for the speed tile.
                        std::lock_guard<std::mutex> hold(shared_->lock);
                        shared_->connections = 1;
                        counted = true;
                    }
                    measure(before + position, length);
                    return true;
                },
                kReplySeconds,
                [&](const HttpReply &header)
                {
                    status = header.status;
                    if (status == 206 && offset > 0)
                        return true;
                    if (status == 200)
                    {
                        // The whole file (the NAS ignored the range): from the start.
                        if (offset > 0 && ::lseek(fd, 0, SEEK_SET) < 0)
                            return false;
                        position = 0;
                        return true;
                    }
                    return false;
                },
                [this] { return stopping(); });
            if (counted)
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->connections = 0;
            }
            if (!write_failed && !buffer_.empty() && !flush(fd))
            {
                write_failed = true;
                write_error = errno;
            }
            if (write_failed)
            {
                *why = errno_text("Couldn't write to the drive", write_error);
                return Result::failed;
            }
            if (reply.complete && position == size)
            {
                // Trailing bytes from an older, longer file go.
                if (::ftruncate(fd, static_cast<off_t>(size)) != 0)
                {
                    *why = errno_text("Couldn't finish a file", errno);
                    return Result::failed;
                }
                return Result::ok;
            }
            if (stopping())
                return Result::stopped;
            if (reply.connected && status != 0 && status != 200 && status != 206)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "The NAS refused a file (HTTP %d)", status);
                *why = text;
                return Result::failed;
            }
            // Cut off: try again, from where it got to. Progress resets the count.
            if (!retry_after(&failures, position > offset + kRefetch,
                             reply.connected ? "The NAS keeps dropping the connection"
                                             : "Lost the connection to the NAS",
                             why))
                return Result::failed;
        }
    }

    // fill() over SMB: the rest of the file, over the copy's one sign-in.
    Result fill_smb(int fd, const std::string &remote, std::uint64_t size, std::uint64_t before,
                    std::string *why)
    {
        if (!smb_)
            smb_ = std::make_unique<SmbSession>(connection_, kReplySeconds);
        int failures = 0;
        while (true)
        {
            std::uint64_t offset = 0;
            bool whole = false;
            if (!resume_at(fd, size, &offset, &whole, why))
                return Result::failed;
            if (whole)
                return Result::ok;
            if (stopping())
                return Result::stopped;

            std::uint64_t position = offset;
            std::uint64_t actual = size;
            bool write_failed = false;
            int write_error = 0;
            buffer_.clear();
            window_start_ = now_ms();
            window_bytes_ = 0;
            bool counted = false;
            SmbFailure failure;
            const bool complete = smb_->stream(
                remote, offset, &actual,
                [&](const char *data, std::size_t length)
                {
                    if (stopping() || actual != size)
                        return false;
                    buffer_.insert(buffer_.end(), data, data + length);
                    position += length;
                    if (buffer_.size() >= kWriteBlock && !flush(fd))
                    {
                        write_failed = true;
                        write_error = errno;
                        return false;
                    }
                    if (!counted)
                    {
                        // One connection, for the speed tile.
                        std::lock_guard<std::mutex> hold(shared_->lock);
                        shared_->connections = 1;
                        counted = true;
                    }
                    measure(before + position, length);
                    return true;
                },
                &failure);
            if (counted)
            {
                std::lock_guard<std::mutex> hold(shared_->lock);
                shared_->connections = 0;
            }
            if (!write_failed && !buffer_.empty() && !flush(fd))
            {
                write_failed = true;
                write_error = errno;
            }
            if (write_failed)
            {
                *why = errno_text("Couldn't write to the drive", write_error);
                return Result::failed;
            }
            if (actual != size)
            {
                *why = "A file changed on the NAS since the library was read: refresh it and try again";
                return Result::failed;
            }
            if (complete && position == size)
            {
                if (::ftruncate(fd, static_cast<off_t>(size)) != 0)
                {
                    *why = errno_text("Couldn't finish a file", errno);
                    return Result::failed;
                }
                return Result::ok;
            }
            if (stopping())
                return Result::stopped;
            using Kind = SmbFailure::Kind;
            if (failure.kind == Kind::sign_in || failure.kind == Kind::no_share ||
                failure.kind == Kind::not_found || failure.kind == Kind::denied ||
                failure.kind == Kind::protocol || failure.kind == Kind::address)
            {
                *why = "The NAS refused a file (" + smb_failure_brief(failure) + ")";
                return Result::failed;
            }
            if (!retry_after(&failures, position > offset + kRefetch,
                             failure.kind == Kind::timeout ? "The NAS keeps going quiet"
                                                           : "Lost the connection to the NAS",
                             why))
                return Result::failed;
        }
    }

    // Progress and speed (smoothed over about two seconds).
    void measure(std::uint64_t done, std::size_t length)
    {
        window_bytes_ += length;
        const std::uint64_t now = now_ms();
        const std::uint64_t elapsed = now - window_start_;
        std::lock_guard<std::mutex> hold(shared_->lock);
        if (Item *item = shared_->item(entry_))
            item->done = done;
        if (elapsed >= 500)
        {
            const double rate = static_cast<double>(window_bytes_) * 1000.0 /
                                static_cast<double>(elapsed);
            shared_->rate = shared_->rate <= 0.0 ? rate : shared_->rate * 0.75 + rate * 0.25;
            window_start_ = now;
            window_bytes_ = 0;
        }
    }

    bool smb() const
    {
        return connection_.type == ConnectionType::smb;
    }

    // The helper's SMB sign-in: the share from the games folder, "guest"
    // without a user name.
    SmbSignIn sign_in() const
    {
        SmbSignIn result;
        result.share = smb_location(folder_path(connection_)).share;
        split_smb_user(connection_.username, &result.domain, &result.user);
        if (result.user.empty())
            result.user = "guest";
        result.password = connection_.password;
        return result;
    }

    Connection connection_;
    SharedPtr shared_;
    std::string entry_;
    std::string staging_;
    std::unique_ptr<SmbSession> smb_; // in-app copies over SMB: one sign-in for the game
    std::vector<char> buffer_;
    bool helper_unavailable_ = false; // the copy helper couldn't start: in-app
    bool batch_ok_ = true;            // the copy helper takes batches of small files
    bool helper_used_ = false;        // the copy helper has copied for this game
    int helper_streams_ = 0;          // its connections for this game (0: not chosen yet)
    std::uint64_t unsynced_ = 0;     // flush(): bytes written since the last fsync
    std::uint64_t last_sync_ms_ = 0; // flush(): when it last fsync'd
    std::uint64_t window_start_ = 0;
    std::uint64_t window_bytes_ = 0;
    std::mutex measure_lock_; // the speed window, shared by a big file's connections
    std::atomic<std::uint64_t> last_report_ms_{0}; // throttles the per-connection line
};

// ---- the queue's worker ----

// Ends the worker in `state` (lock held by the caller's scope is not held here).
void stop_queue(const SharedPtr &shared, QueueState state, const std::string &message)
{
    std::lock_guard<std::mutex> hold(shared->lock);
    shared->state = shared->queue.empty() ? QueueState::idle : state;
    shared->message = shared->queue.empty() ? std::string() : message;
    shared->phase = TransferState::idle;
    shared->detail.clear();
    shared->rate = 0.0;
    shared->worker = false;
    shared->save();
}

void run_queue(const SharedPtr &shared)
{
    while (true)
    {
        Item head;
        Connection connection;
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            if (shared->queue.empty())
            {
                shared->state = QueueState::idle;
                shared->message.clear();
                shared->phase = TransferState::idle;
                shared->worker = false;
                shared->save();
                return;
            }
            head = shared->queue.front();
            connection = shared->connection;
        }
        if (shared->stop.load())
            return;
        if (shared->pause.load())
            return stop_queue(shared, QueueState::paused, "");

        // Before each game: is there still room for what it needs? (Something
        // else may have filled the drive since it was queued.)
        std::uint64_t free = 0;
        const std::uint64_t need = head.size() > head.done ? head.size() - head.done : 0;
        if (shared->space && shared->space(head.plan.destination, &free) && need > free)
        {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "Not enough space on %s for %s: the game needs %.1f GB, %.1f GB is free",
                          destination_name(head.plan.destination).c_str(),
                          head.plan.title.c_str(), gigabytes(need), gigabytes(free));
            stop_queue(shared, QueueState::blocked, text);
            ps5_notify(std::string("The copy queue stopped. ") + text + ".");
            return;
        }

        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->state = QueueState::running;
            shared->message.clear();
        }
        std::string why;
        Copier copier(connection, shared, head.plan.entry);
        const Outcome outcome = copier.run(&why);
        switch (outcome)
        {
        case Outcome::done:
        {
            {
                std::lock_guard<std::mutex> hold(shared->lock);
                for (std::size_t i = 0; i < shared->queue.size(); ++i)
                    if (shared->queue[i].plan.entry == head.plan.entry)
                        shared->queue.erase(shared->queue.begin() + static_cast<long>(i));
                shared->finished_entry = head.plan.entry;
                shared->finished_destination = head.plan.destination;
                if (copier.held_back())
                    shared->waiting_for_close.push_back(
                        {head.plan.entry, head.plan.destination});
                shared->phase = TransferState::idle;
                shared->save();
            }
            std::string after = ". The game will be on your home screen shortly.";
            if (!why.empty())
                after = ". " + why;
            else if (copier.held_back())
                after = ". Close NASBridge to add the game to your home screen.";
            ps5_notify(head.plan.title + " is copied to " +
                       destination_name(head.plan.destination) + after);
            continue; // the next game, after its space check
        }
        case Outcome::paused:
            return stop_queue(shared, QueueState::paused, "");
        case Outcome::closing:
            return; // the saved queue comes back paused
        case Outcome::failed:
            stop_queue(shared, QueueState::failed, why);
            ps5_notify("Copying " + head.plan.title + " stopped: " + why);
            return;
        }
    }
}

// ---- deleting what a game had copied (it left the queue) ----

// Returns "" or why something couldn't be deleted. `progress` hears how far
// it is (0..1) every so often: a big game is tens of thousands of files.
std::string discard_files(const CopyPlan &plan, const std::function<void(float)> &progress)
{
    const std::string incoming = plan.destination + "/" + kIncoming;
    const std::string staging = incoming + "/" + plan.name;
    std::string problem;
    if (plan.as_image)
    {
        // One file, whatever the game holds.
        if (::unlink(staging.c_str()) != 0 && errno != ENOENT)
            problem = errno_text(("Couldn't delete " + plan.name).c_str(), errno);
        ::rmdir(incoming.c_str());
        return problem;
    }
    const std::size_t total = plan.files.size() + plan.folders.size();
    std::size_t done = 0;
    const auto step = [&]
    {
        if (++done % 64 == 0 && progress)
            progress(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)));
    };
    for (const CopyFile &file : plan.files)
    {
        step();
        const std::string local = plan.image ? staging : staging + "/" + file.path;
        ::unlink((local + kPartSuffix).c_str()); // a big file still coming in
        if (::unlink(local.c_str()) != 0 && errno != ENOENT && problem.empty())
            problem = errno_text(("Couldn't delete " + (file.path.empty() ? plan.name : file.path))
                                     .c_str(),
                                 errno);
    }
    // Folders deepest first.
    std::vector<std::string> folders = plan.folders;
    std::sort(folders.begin(), folders.end(),
              [](const std::string &a, const std::string &b)
              {
                  const auto depth = [](const std::string &s)
                  { return std::count(s.begin(), s.end(), '/'); };
                  return depth(a) != depth(b) ? depth(a) > depth(b) : a > b;
              });
    for (const std::string &folder : folders)
    {
        step();
        if (::rmdir((staging + "/" + folder).c_str()) != 0 && errno != ENOENT && problem.empty())
            problem = errno_text(("Couldn't delete the folder " + folder).c_str(), errno);
    }
    if (!plan.image && ::rmdir(staging.c_str()) != 0 && errno != ENOENT && problem.empty())
        problem = errno_text("Couldn't delete the game's folder", errno);
    ::rmdir(incoming.c_str()); // only when nothing else is in it
    forget_part(plan.entry);
    return problem;
}

void run_discard(const SharedPtr &shared, const CopyPlan &plan)
{
    const std::string problem = discard_files(plan,
                                              [&](float done)
                                              {
                                                  std::lock_guard<std::mutex> hold(shared->lock);
                                                  shared->discarded[plan.entry] = done;
                                              });
    {
        std::lock_guard<std::mutex> hold(shared->lock);
        auto &list = shared->discarding;
        list.erase(std::remove(list.begin(), list.end(), plan.entry), list.end());
        shared->discarded.erase(plan.entry);
    }
    if (!problem.empty())
        ps5_notify("Couldn't delete the copied files of " + plan.title + ": " + problem);
}

// ---- deleting a game on the PS5 ----

// Through ShadowMountPlus itself, asking as NASBridge (a newer ShadowMountPlus
// takes that while NASBridge is open): it deletes the files and removes the
// game from the home screen the standard way. Returns false, with nothing
// changed, when it won't (the helper does it then).
int g_uninstall_wait_ms = 60000; // how long an uninstall waits out a busy ShadowMountPlus

// ShadowMountPlus refuses an uninstall while it scans or runs another storage
// job, and it scans straight after deleting a game's files: so it is asked
// again until it's free. A game already off the home screen (that scan can
// take it off) counts as done.
bool uninstall_when_idle(const std::string &title_id, std::string *why, std::string *reason)
{
    for (int waited = 0;; waited += 1000)
    {
        if (smp_uninstall(title_id, why, reason))
            return true;
        if (*reason != "scanner_busy" && *reason != "runtime_busy")
            break;
        if (waited >= g_uninstall_wait_ms)
            break;
        sleep_ms(1000);
    }
    std::vector<SmpGame> games;
    std::string list_why;
    if (!smp_games(&games, &list_why))
        return false;
    for (const SmpGame &game : games)
        if (game.title_id == title_id && game.installed)
            return false;
    return true;
}

// False when ShadowMountPlus refused before changing anything; *refused says
// why, plainly, *hint what to do about it, and *reason_code its error_reason.
// The folder a game's folder is in ("/data/homebrew" for "/data/homebrew/X").
std::string parent_folder(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string("/") : path.substr(0, slash);
}

void removal_progress(const SharedPtr &shared, std::uint64_t done, std::uint64_t total)
{
    std::lock_guard<std::mutex> hold(shared->lock);
    shared->removal.done_bytes = done;
    shared->removal.total_bytes = total;
}

// A folder game's files, ShadowMountPlus having removed it from the home
// screen: the helper moves the folder into <its parent>/.nasbridge-deleting
// (where ShadowMountPlus never looks), then deletes that, several files at
// once, with progress. (root_only: a delete that stopped short, finished.)
void delete_folder_files(const SharedPtr &shared, const std::string &title_id,
                         const std::string &path, bool root_only = false)
{
    const std::string root = root_only ? path : parent_folder(path);
    {
        std::lock_guard<std::mutex> hold(shared->lock);
        shared->add_unfinished(root); // until the hidden folder is empty
        shared->leftover_title = title_id;
        shared->leftover_root = root;
        shared->removal.leftover = true;
        shared->removal.done_bytes = 0;
        shared->removal.total_bytes = 0;
    }
    shared->set_removal(TransferState::deleting, "Removing game files");
    const auto progress = [&](std::uint64_t done, std::uint64_t total)
    { removal_progress(shared, done, total); };
    const DeleteResult result = root_only
                                    ? finish_deletes_with_helper(root, progress, g_delete_threads)
                                    : delete_files_with_helper(title_id, path, progress,
                                                               g_delete_threads);
    if (result.ok)
    {
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->drop_unfinished(root);
            shared->leftover_title.clear();
            shared->leftover_root.clear();
            shared->removal.leftover = false;
        }
        // ShadowMountPlus lists the game until it next scans (its own delete
        // asks for that scan): ask, so the library sees it gone.
        std::string why, deferred;
        smp_scan(true, &why, &deferred);
        return shared->set_removal(TransferState::deleted, "Deleted from the PS5");
    }
    const std::string said =
        result.why.empty() ? std::string("the delete helper stopped") : result.why;
    if (result.stage == "request" || result.stage == "loader" || result.stage == "helper")
    {
        // Nothing was moved or deleted: it's where it was, just off the
        // home screen, and Delete from PS5 tries again.
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->leftover_title.clear();
            shared->leftover_root.clear();
            shared->removal.leftover = false;
        }
        return shared->set_removal(TransferState::failed, "Couldn't delete the game: " + said);
    }
    shared->set_removal(TransferState::failed,
                        "The game is off the home screen, but not all of its files were deleted (" + said +
                            "). Choose Delete the rest to finish; otherwise NASBridge finishes "
                            "the next time NASBridge starts.");
}

// At start-up: what deletes cut short left in .nasbridge-deleting, finished
// quietly (another delete waits for it).
void finish_unfinished(const SharedPtr &shared)
{
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> hold(shared->lock);
        roots = shared->unfinished;
    }
    for (const std::string &root : roots)
    {
        const DeleteResult result = finish_deletes_with_helper(root, nullptr, g_delete_threads);
        if (result.ok)
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->drop_unfinished(root);
        }
    }
    std::lock_guard<std::mutex> hold(shared->lock);
    shared->finishing = false;
}

bool remove_with_smp(const SharedPtr &shared, const SmpGame &game, std::string *refused,
                     std::string *hint, std::string *reason_code)
{
    // A folder game: ShadowMountPlus removes it from the home screen (and
    // releases its files if they're attached), then the helper deletes the
    // files many at once. ShadowMountPlus's own delete goes one at a time,
    // which for tens of thousands of files takes minutes.
    if (game.source_type == "folder")
    {
        std::string why, reason;
        shared->set_removal(TransferState::deleting, "Removing the game from the home screen");
        if (!uninstall_when_idle(game.title_id, &why, &reason))
        {
            const std::string text = smp_reason_text(reason);
            *refused = text.empty() ? why : text; // nothing changed yet
            *hint = smp_reason_hint(reason);
            *reason_code = reason;
            return false;
        }
        delete_folder_files(shared, game.title_id, game.path);
        return true;
    }

    std::string why;
    std::string reason;
    const auto said = [&]
    {
        const std::string text = smp_reason_text(reason);
        return text.empty() ? why : text;
    };
    const bool pkg = game.source_type == "pkg"; // nothing of its own to delete
    if (!pkg)
    {
        shared->set_removal(TransferState::deleting, "Removing game files");
        int job_id = 0;
        bool missing = false;
        if (!smp_delete_source(game.title_id, &job_id, &missing, &why, &reason))
        {
            *refused = said(); // before anything was deleted
            *hint = smp_reason_hint(reason);
            *reason_code = reason;
            return false;
        }
        if (!missing)
        {
            SmpJob job;
            int unanswered = 0;
            while (true)
            {
                sleep_ms(300);
                if (!smp_job(job_id, &job, &why))
                {
                    if (++unanswered > 20)
                    {
                        shared->set_removal(TransferState::failed,
                                            "Couldn't follow the delete: " + why);
                        return true;
                    }
                    continue;
                }
                unanswered = 0;
                {
                    std::lock_guard<std::mutex> hold(shared->lock);
                    shared->removal.done_bytes = job.processed_bytes;
                    shared->removal.total_bytes = job.total_bytes;
                }
                if (!job.active)
                    break;
            }
            if (job.state != "completed")
            {
                // Refused inside its job before deleting anything (it checks
                // again there): the helper can still do it.
                if (job.processed_bytes == 0 &&
                    (job.reason == "game_active" || job.reason == "runtime_prepared"))
                {
                    reason = job.reason;
                    why = job.error;
                    *refused = said();
                    *hint = smp_reason_hint(reason);
                    *reason_code = reason;
                    return false;
                }
                shared->set_removal(TransferState::failed,
                                    "Couldn't delete the game: " +
                                        (job.error.empty() ? job.state : job.error));
                return true;
            }
        }
    }
    shared->set_removal(TransferState::deleting, "Removing the game from the home screen");
    if (!uninstall_when_idle(game.title_id, &why, &reason))
    {
        if (pkg)
        {
            *refused = said(); // nothing changed yet
            *hint = smp_reason_hint(reason);
            *reason_code = reason;
            return false;
        }
        shared->set_removal(TransferState::failed,
                            "The game files are deleted, but removing the game from the home screen failed: " +
                                said() + ". " + smp_reason_hint(reason));
        return true;
    }
    shared->set_removal(TransferState::deleted, "Deleted from the PS5");
    return true;
}

void remove_game(const SharedPtr &shared, const std::string &title_id)
{
    // ShadowMountPlus's list is still readable while NASBridge is open. From
    // it we get the game's files and whether it's running. The delete goes to
    // ShadowMountPlus first: from 1.7beta6 it changes other games while a
    // homebrew app (a LAPY title ID, as NASBridge has) is open. An older one
    // refuses while NASBridge counts as a running game, so the helper payload
    // does it then.
    std::string why;
    std::vector<SmpGame> games;
    if (!smp_games(&games, &why))
        return shared->set_removal(TransferState::failed, why);
    const SmpGame *game = nullptr;
    for (const SmpGame &candidate : games)
        if (candidate.title_id == title_id)
            game = &candidate;
    if (game == nullptr)
    {
        // Already off the home screen, with files left from a delete that
        // stopped: finish deleting them.
        std::string leftover;
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            if (shared->leftover_title == title_id)
                leftover = shared->leftover_root;
        }
        if (!leftover.empty())
            return delete_folder_files(shared, title_id, leftover, true);
        return shared->set_removal(TransferState::failed, "ShadowMountPlus doesn't know this game");
    }

    // ShadowMountPlus does it all when it can: it releases a game whose files
    // are attached ("mounted": a game just added, or an image game that has
    // been played) and refuses a running one. When it says no, the helper
    // payload does, but only for a game that isn't attached: it can't release
    // one.
    std::string refused, hint, reason_code;
    if (remove_with_smp(shared, *game, &refused, &hint, &reason_code))
        return;
    if (game->mounted)
    {
        // "A game is running": NASBridge itself, to a ShadowMountPlus older
        // than 1.7beta6 (which changes no game while an app is open); to a
        // newer one, this game shares files with NASBridge. Its API doesn't
        // say which version it is, so the message covers both.
        if (reason_code == "game_active")
            return shared->set_removal(
                TransferState::failed,
                "ShadowMountPlus won't change this game while NASBridge is open. If it's older "
                "than 1.7beta6-fix2, update it; otherwise restart the PS5, then delete the game.");
        return shared->set_removal(TransferState::failed,
                                   "ShadowMountPlus wouldn't delete the game: " + refused + ". " +
                                       hint);
    }

    // An installed PKG has no source files of its own: the helper only
    // uninstalls it ("" path). Otherwise the helper deletes its folder or
    // image, then uninstalls it.
    const std::string path = game->source_type == "pkg" ? std::string() : game->path;
    shared->set_removal(TransferState::deleting, path.empty() ? "Removing the game from the home screen"
                                                             : "Removing game files");
    const DeleteResult result = delete_with_helper(
        title_id, path,
        [&](std::uint64_t done, std::uint64_t total)
        {
            std::lock_guard<std::mutex> hold(shared->lock);
            shared->removal.done_bytes = done;
            shared->removal.total_bytes = total;
            if (total > 0 && done >= total)
                shared->removal.message = "Removing the game from the home screen";
        });
    if (result.ok)
        return shared->set_removal(TransferState::deleted, "Deleted from the PS5");

    // Where it stopped decides what, if anything, is left behind.
    std::string message = result.why.empty() ? "the delete helper failed" : result.why;
    if (result.stage == "uninstall")
        message = "The game files are deleted, but removing the game from the home screen failed: " + message;
    else
        message = "Couldn't delete the game: " + message;
    shared->set_removal(TransferState::failed, message);
}

// ---- threads ----

struct Work
{
    enum class Kind
    {
        queue,
        discard,
        remove,
        finish, // what deletes cut short left, at start-up
    };
    Kind kind = Kind::queue;
    SharedPtr shared;
    CopyPlan plan;        // discard
    std::string title_id; // remove
};

void *work_main(void *argument)
{
    Work *work = static_cast<Work *>(argument);
    keep_off_cpu(work->shared->ui_cpu);
    std::string missing;
    if (work->kind != Work::Kind::remove && work->kind != Work::Kind::finish &&
        !file_calls_available(&missing))
    {
        // Calling a function that isn't there would crash NASBridge.
        if (work->kind == Work::Kind::queue)
            stop_queue(work->shared, QueueState::failed, "This PS5 is missing " + missing);
        delete work;
        return nullptr;
    }
    switch (work->kind)
    {
    case Work::Kind::queue:
        run_queue(work->shared);
        break;
    case Work::Kind::discard:
        run_discard(work->shared, work->plan);
        break;
    case Work::Kind::remove:
        remove_game(work->shared, work->title_id);
        break;
    case Work::Kind::finish:
        finish_unfinished(work->shared);
        break;
    }
    delete work;
    return nullptr;
}

bool launch(Work *work)
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, &work_main, work);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete work;
        return false;
    }
    return true;
}

// Starts the queue's worker (lock held). False when it couldn't.
bool start_worker(const SharedPtr &shared)
{
    if (shared->worker)
        return true;
    shared->pause.store(false);
    shared->worker = true;
    shared->state = QueueState::running;
    shared->message.clear();
    Work *work = new Work;
    work->kind = Work::Kind::queue;
    work->shared = shared;
    if (!launch(work))
    {
        shared->worker = false;
        shared->state = QueueState::failed;
        shared->message = "Couldn't start copying";
        return false;
    }
    return true;
}

// Asks ShadowMountPlus how much room a destination has.
bool ask_space(const std::string &destination, std::uint64_t *free)
{
    const StorageSpace space = shadowmount_storage();
    for (const Destination &place : space.destinations)
        if (place.path == destination)
        {
            *free = place.free;
            return true;
        }
    if (destination == "/data/homebrew" && space.known)
    {
        *free = space.free;
        return true;
    }
    return false; // can't tell: the copy goes ahead
}

} // namespace

// ---- Transfers ----

Transfers::Transfers() : shared_(std::make_shared<Shared>())
{
    shared_->ui_cpu = current_cpu();
    shared_->space = ask_space;
}

Transfers::~Transfers()
{
    shared_->stop.store(true);
}

void Transfers::set_space_check(SpaceCheck check)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    shared_->space = std::move(check);
}

void Transfers::load()
{
    std::vector<SavedItem> saved;
    std::string text;
    if (save::read_file(queue_file(), &text, 256u * 1024u * 1024u))
    {
        if (!queue_from_text(text, &saved))
            saved.clear();
    }
    std::lock_guard<std::mutex> hold(shared_->lock);
    shared_->queue.clear();
    for (const SavedItem &item : saved)
        shared_->queue.push_back({item.plan, item.estimate, item.done, item.started});
    shared_->state = shared_->queue.empty() ? QueueState::idle : QueueState::paused;
    shared_->message = shared_->queue.empty() ? "" : "Stopped when NASBridge closed";
    shared_->save();

    // Deletes cut short last time (NASBridge closed, a crash, a power cut):
    // their files wait in .nasbridge-deleting; finish them now, quietly.
    shared_->unfinished.clear();
    std::string roots;
    if (save::read_file(unfinished_file(), &roots, 64 * 1024))
    {
        std::size_t at = 0;
        while (at < roots.size())
        {
            const std::size_t end = roots.find('\n', at);
            const std::string root =
                roots.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!root.empty() && root[0] == '/')
                shared_->unfinished.push_back(root);
            at = end == std::string::npos ? roots.size() : end + 1;
        }
    }
    if (!shared_->unfinished.empty())
    {
        Work *work = new Work;
        work->kind = Work::Kind::finish;
        work->shared = shared_;
        shared_->finishing = launch(work);
    }
}

bool Transfers::enqueue(const Connection &connection, const NasGame &game, const std::string &title,
                        const std::string &destination, std::string *why)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (shared_->item(game.entry) != nullptr)
    {
        *why = "That game is already in the queue";
        return false;
    }
    if (std::find(shared_->discarding.begin(), shared_->discarding.end(), game.entry) !=
        shared_->discarding.end())
    {
        *why = "The game's earlier copy is still being deleted";
        return false;
    }
    if (!shared_->queue.empty() &&
        shared_->queue.front().plan.nas != nas_identity(connection))
    {
        *why = "The queue copies from another NAS connection";
        return false;
    }
    CopyPlan plan;
    plan.entry = game.entry;
    plan.title = title;
    plan.title_id = game.title_id;
    plan.image = game.image;
    plan.source = copy_source(connection, game);
    plan.destination = destination;
    plan.name = copy_name(game);
    plan.nas = nas_identity(connection);
    if (!game.image && copy_as_image())
    {
        plan.as_image = true;
        plan.name += ".exfat";
    }
    if (!safe_name(plan.name) || plan.entry.find('\n') != std::string::npos ||
        plan.title.find('\n') != std::string::npos)
    {
        *why = "NASBridge can't copy a game with this name: \"" + plan.name + "\"";
        return false;
    }
    for (const Item &queued : shared_->queue)
        if (queued.plan.destination == destination && queued.plan.name == plan.name)
        {
            *why = "A game called \"" + plan.name + "\" is already queued for this drive";
            return false;
        }
    shared_->queue.push_back({plan, game.size, 0, false});
    // A finished delete of this game is over once it's queued again: its
    // "Deleted" (kept while ShadowMountPlus still lists the game) mustn't
    // stay on the game's page once the new copy is on the PS5.
    if (shared_->removal.entry == game.entry && shared_->removal.state == TransferState::deleted)
        shared_->removal = TransferStatus{};
    shared_->connection = connection;
    if (shared_->finished_entry == game.entry)
        shared_->finished_entry.clear();
    auto &waiting = shared_->waiting_for_close;
    waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                 [&](const QueueStatus::Waiting &item)
                                 { return item.entry == game.entry; }),
                  waiting.end());
    shared_->save();
    // An empty queue starts at once; a paused or stopped one waits for Resume.
    if (shared_->state == QueueState::idle && !start_worker(shared_))
    {
        *why = shared_->message;
        return false;
    }
    return true;
}

bool Transfers::pause()
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (!shared_->worker)
        return false;
    shared_->pause.store(true);
    shared_->detail = "Pausing";
    return true;
}

bool Transfers::resume(const Connection &connection, std::string *why)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (shared_->worker)
    {
        *why = "It's already copying";
        return false;
    }
    if (shared_->queue.empty())
    {
        *why = "The queue is empty";
        return false;
    }
    for (const Item &queued : shared_->queue)
        if (queued.plan.nas != nas_identity(connection))
        {
            *why = queued.plan.title + " came from another NAS connection (" + queued.plan.nas +
                   ")";
            return false;
        }
    shared_->connection = connection;
    if (!start_worker(shared_))
    {
        *why = shared_->message;
        return false;
    }
    return true;
}

bool Transfers::move(int index, int delta, std::string *why)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    const int to = index + delta;
    const int count = static_cast<int>(shared_->queue.size());
    if (index < 0 || index >= count || to < 0 || to >= count || (delta != 1 && delta != -1))
    {
        *why = "It can't move that way";
        return false;
    }
    if (shared_->worker && (index == 0 || to == 0))
    {
        *why = "The game being copied stays first: pause the queue to change it";
        return false;
    }
    std::swap(shared_->queue[static_cast<std::size_t>(index)],
              shared_->queue[static_cast<std::size_t>(to)]);
    // A queue stopped for the old front game's sake is just paused now.
    if ((index == 0 || to == 0) && shared_->state != QueueState::running)
    {
        shared_->state = QueueState::paused;
        shared_->message.clear();
    }
    shared_->save();
    return true;
}

bool Transfers::remove(int index, std::string *why)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (index < 0 || index >= static_cast<int>(shared_->queue.size()))
    {
        *why = "It isn't in the queue";
        return false;
    }
    if (shared_->worker && index == 0)
    {
        *why = "Pause the queue first, then remove it";
        return false;
    }
    const Item item = shared_->queue[static_cast<std::size_t>(index)];
    shared_->queue.erase(shared_->queue.begin() + index);
    const QueueState before = shared_->state;
    if (shared_->queue.empty())
    {
        shared_->state = QueueState::idle;
        shared_->message.clear();
    }
    else if (index == 0 && before == QueueState::failed)
    {
        shared_->state = QueueState::paused; // the error was the removed game's
        shared_->message.clear();
    }
    shared_->save();
    if (item.started || item.plan.listed)
    {
        // Its copied files go, on a thread of their own.
        shared_->discarding.push_back(item.plan.entry);
        Work *work = new Work;
        work->kind = Work::Kind::discard;
        work->shared = shared_;
        work->plan = item.plan;
        if (!launch(work))
        {
            shared_->discarding.pop_back();
            *why = "Removed, but its copied files couldn't be deleted";
            return false;
        }
    }
    // The game that didn't fit is gone: the next one gets its turn.
    if (index == 0 && before == QueueState::blocked && !shared_->queue.empty())
        start_worker(shared_);
    return true;
}

bool Transfers::remove_installed(const std::string &title_id, const std::string &entry,
                                 const std::string &title, std::string *why)
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    if (shared_->removal.state == TransferState::deleting)
    {
        *why = "Another game is being deleted";
        return false;
    }
    if (title_id.empty())
    {
        *why = "This game's title ID isn't known";
        return false;
    }
    if (shared_->finishing)
    {
        *why = "NASBridge is finishing an earlier delete: try again in a moment";
        return false;
    }
    TransferStatus status;
    status.kind = TransferKind::remove;
    status.state = TransferState::deleting;
    status.entry = entry;
    status.title = title;
    status.title_id = title_id;
    status.message = "Asking ShadowMountPlus";
    shared_->removal = status;
    // A game just copied stops counting as on its way to the home screen.
    if (shared_->finished_entry == entry)
        shared_->finished_entry.clear();
    auto &waiting = shared_->waiting_for_close;
    waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                 [&](const QueueStatus::Waiting &item) { return item.entry == entry; }),
                  waiting.end());
    Work *work = new Work;
    work->kind = Work::Kind::remove;
    work->shared = shared_;
    work->title_id = title_id;
    if (!launch(work))
    {
        shared_->removal = TransferStatus{};
        *why = "Couldn't start deleting";
        return false;
    }
    return true;
}

QueueStatus Transfers::queue() const
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    QueueStatus status;
    status.state = shared_->state;
    status.message = shared_->message;
    for (const Item &item : shared_->queue)
    {
        QueueEntry entry;
        entry.entry = item.plan.entry;
        entry.title = item.plan.title;
        entry.title_id = item.plan.title_id;
        entry.destination = item.plan.destination;
        entry.size = item.size();
        entry.done = std::min(item.done, item.size());
        entry.started = item.started;
        entry.image = item.plan.one_file();
        status.entries.push_back(entry);
    }
    status.phase = shared_->phase;
    status.detail = shared_->detail;
    status.rate = shared_->rate;
    status.connections = shared_->connections;
    status.conn_report = shared_->conn_report;
    status.files_done = shared_->files_done;
    status.files_total = shared_->files_total;
    status.discarding = shared_->discarding;
    status.discarded = shared_->discarded;
    status.finished_entry = shared_->finished_entry;
    status.finished_destination = shared_->finished_destination;
    status.waiting_for_close = shared_->waiting_for_close;
    return status;
}

TransferStatus Transfers::removal() const
{
    std::lock_guard<std::mutex> hold(shared_->lock);
    return shared_->removal;
}

} // namespace hui::concepts
