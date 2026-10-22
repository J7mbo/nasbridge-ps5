// NASBridge - A drive plugged into the PS5 as the place games are read from.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/drive.hpp"

#include "concepts/json_scan.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/storage.hpp"

#include "core/save_file.hpp"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace hui::concepts
{

namespace
{

std::string g_prefix; // tests: where /mnt is

// What the drive's real path is (tests put /mnt under a folder of their own).
std::string real(const std::string &path)
{
    return g_prefix.empty() ? path : g_prefix + path;
}

bool skipped(const std::string &name)
{
    if (name.empty() || name[0] == '.')
        return true;
    std::string lower = name;
    for (char &c : lower)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return lower == "system volume information" || lower == "$recycle.bin" ||
           lower == "@eadir" || lower == "#recycle";
}

std::string parent_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

std::string leaf_of(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "/homebrew/.Astro Bot" for "/mnt/usb0/homebrew/.Astro Bot".
std::string on_drive(const std::string &path)
{
    const std::string root = drive_root_of(path);
    return root.empty() ? path : path.substr(root.size());
}

// Something at `path` (a file or a folder), by opening it: NASBridge's
// sandbox may refuse stat.
bool there(const std::string &path)
{
    struct stat info;
    if (::stat(real(path).c_str(), &info) == 0)
        return true;
    const int fd = ::open(real(path).c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

bool is_folder(const std::string &path)
{
    DIR *dir = ::opendir(real(path).c_str());
    if (dir == nullptr)
        return false;
    ::closedir(dir);
    return true;
}

// A file's size: stat, or (where the sandbox refuses it) its end.
bool size_of(const std::string &path, std::uint64_t *size, std::string *modified)
{
    struct stat info;
    if (::stat(real(path).c_str(), &info) == 0)
    {
        *size = static_cast<std::uint64_t>(info.st_size);
        *modified = std::to_string(static_cast<long long>(info.st_mtime));
        return true;
    }
    const int fd = ::open(real(path).c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    const off_t end = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    if (end < 0)
        return false;
    *size = static_cast<std::uint64_t>(end);
    modified->clear();
    return true;
}

// A game folder's title ID, from its sce_sys/param.json; "" when it can't be read.
std::string title_of(const std::string &folder)
{
    std::string json;
    bool too_big = false;
    int error = 0;
    if (!drive_read_file(folder + "/sce_sys/param.json", 256 * 1024, &json, &too_big, &error))
        return "";
    return json_text(json.data(), json.data() + json.size(), "titleId");
}

// ---- the list of hidden copies ----

std::mutex g_lock;
bool g_loaded = false;
bool g_warning_off = false;
std::vector<HiddenCopy> g_hidden;

std::string list_file()
{
    return data_root() + "/hidden.txt";
}

// Loads the list once (g_lock held).
void load()
{
    if (g_loaded)
        return;
    g_loaded = true;
    g_hidden.clear();
    std::string text;
    if (!save::read_file(list_file(), &text, 256 * 1024))
        return;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> field;
        std::size_t from = 0;
        while (true)
        {
            const std::size_t tab = line.find('\t', from);
            field.push_back(line.substr(from, tab == std::string::npos ? std::string::npos
                                                                       : tab - from));
            if (tab == std::string::npos)
                break;
            from = tab + 1;
        }
        if (field.size() == 2 && field[0] == "warn")
            g_warning_off = field[1] == "0";
        else if (field.size() == 6 && field[0] == "H" && !field[1].empty())
            g_hidden.push_back({field[1], field[4], field[5], field[2] == "1", field[3] == "1"});
    }
}

// Writes it (g_lock held).
void store()
{
    std::string text = std::string("warn\t") + (g_warning_off ? "0" : "1") + "\n";
    for (const HiddenCopy &copy : g_hidden)
        text += "H\t" + copy.title_id + "\t" + (copy.image ? "1" : "0") + "\t" +
                (copy.unhide ? "1" : "0") + "\t" + copy.path + "\t" + copy.hidden + "\n";
    save::write_atomic(list_file(), text);
}

void forget(const std::string &title_id)
{
    for (std::size_t i = 0; i < g_hidden.size();)
        if (g_hidden[i].title_id == title_id)
            g_hidden.erase(g_hidden.begin() + static_cast<long>(i));
        else
            ++i;
}

// Whether the folder the copy was hidden in is on a drive that's plugged in
// now, in the same place (so a copy missing from it is really gone).
bool its_folder_is_here(const HiddenCopy &copy)
{
    const std::string root = drive_root_of(copy.path);
    return !root.empty() && is_folder(root) && is_folder(parent_of(copy.path));
}

} // namespace

// ---- the drives ----

const std::vector<std::string> &drive_roots()
{
    static const std::vector<std::string> roots = {
        "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",
        "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1"};
    return roots;
}

std::vector<std::string> drive_roots_present()
{
    std::vector<std::string> present;
    for (const std::string &root : drive_roots())
        if (is_folder(root))
            present.push_back(root);
    return present;
}

std::string drive_root_of(const std::string &path)
{
    for (const std::string &root : drive_roots())
        if (path.compare(0, root.size(), root) == 0 &&
            (path.size() == root.size() || path[root.size()] == '/'))
            return root;
    return "";
}

bool drive_root_present(const std::string &path)
{
    const std::string root = drive_root_of(path);
    return !root.empty() && is_folder(root);
}

// Declared in storage.hpp; here so that everything naming a drive needs only this file.
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

std::string drive_name(const std::string &root)
{
    return destination_name(root);
}

std::string drive_real_path(const std::string &path)
{
    return real(path);
}

void set_drive_prefix(const std::string &prefix)
{
    g_prefix = prefix;
}

// ---- reading it ----

bool drive_list(const std::string &folder, std::vector<DavEntry> *entries, int *error)
{
    entries->clear();
    std::string base = folder;
    if (base.empty() || base.back() != '/')
        base += '/';
    DIR *dir = ::opendir(real(base).c_str());
    if (dir == nullptr)
    {
        *error = errno;
        return false;
    }
    while (const dirent *item = ::readdir(dir))
    {
        const std::string name = item->d_name;
        if (skipped(name))
            continue;
        DavEntry entry;
        entry.name = name;
        entry.path = base + name;
        if (item->d_type == DT_DIR)
            entry.folder = true;
        else if (item->d_type != DT_REG)
            entry.folder = is_folder(entry.path); // a link, or a file system that doesn't say
        if (!entry.folder && !size_of(entry.path, &entry.size, &entry.modified))
            continue; // gone since it was listed, or unreadable
        entries->push_back(std::move(entry));
    }
    ::closedir(dir);
    return true;
}

bool drive_read_file(const std::string &path, std::size_t max, std::string *out, bool *too_big,
                     int *error)
{
    out->clear();
    *too_big = false;
    const int fd = ::open(real(path).c_str(), O_RDONLY);
    if (fd < 0)
    {
        *error = errno;
        return false;
    }
    std::string chunk(64 * 1024, '\0'); // not on the stack: some threads' are small
    while (true)
    {
        const ssize_t n = ::read(fd, &chunk[0], chunk.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            *error = errno;
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        out->append(chunk.data(), static_cast<std::size_t>(n));
        if (out->size() > max)
        {
            *too_big = true;
            out->clear();
            ::close(fd);
            return false;
        }
    }
    ::close(fd);
    return true;
}

bool drive_read_range(const std::string &path, std::uint64_t offset, std::size_t length,
                      std::string *out, int *error)
{
    out->assign(length, '\0');
    const int fd = ::open(real(path).c_str(), O_RDONLY);
    if (fd < 0)
    {
        *error = errno;
        return false;
    }
    std::size_t have = 0;
    while (have < length)
    {
        const ssize_t n = ::pread(fd, &(*out)[have], length - have,
                                  static_cast<off_t>(offset + have));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            *error = n == 0 ? EIO : errno; // shorter than it should be
            ::close(fd);
            return false;
        }
        have += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

std::string drive_error_brief(const std::string &path, int error)
{
    const std::string root = drive_root_of(path);
    if (root.empty())
        return "not on a USB drive";
    if (!is_folder(root))
        return "the drive isn't connected";
    switch (error)
    {
    case ENOENT:
    case ENOTDIR:
        return "not found";
    case EACCES:
    case EPERM:
        return "access denied";
    case EIO:
    case ENXIO:
        return "the drive didn't answer";
    default:
        return std::strerror(error);
    }
}

// ---- hiding a drive copy ----

bool hide_drive_copy(const std::string &title_id, const std::string &path, bool image,
                     std::string *why)
{
    std::string from = path;
    while (from.size() > 1 && from.back() == '/')
        from.pop_back();
    const std::string hidden = parent_of(from) + "/." + leaf_of(from);
    if (there(hidden))
    {
        *why = "there's already a hidden " + leaf_of(hidden) + " beside it";
        return false;
    }
    if (::rename(real(from).c_str(), real(hidden).c_str()) != 0)
    {
        *why = std::strerror(errno);
        return false;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    forget(title_id);
    g_hidden.push_back({title_id, from, hidden, image, false});
    store();
    return true;
}

bool hidden_copy(const std::string &title_id, HiddenCopy *copy)
{
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    for (const HiddenCopy &item : g_hidden)
        if (item.title_id == title_id)
        {
            *copy = item;
            return true;
        }
    return false;
}

std::vector<HiddenCopy> hidden_copies()
{
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    return g_hidden;
}

bool find_hidden_copy(const HiddenCopy &copy, std::string *where)
{
    const std::string relative = on_drive(copy.hidden);  // "/homebrew/.Astro Bot"
    const std::string folder = parent_of(relative);      // "/homebrew"
    const auto matches = [&](const std::string &candidate)
    {
        if (copy.image)
            return !is_folder(candidate) && there(candidate);
        if (!is_folder(candidate))
            return false;
        const std::string id = title_of(candidate);
        return id.empty() || id == copy.title_id; // unreadable: the name has to do
    };
    // Its own drive first, then the others (it may be in another port now).
    std::vector<std::string> roots = drive_roots_present();
    const std::string own = drive_root_of(copy.hidden);
    for (std::size_t i = 0; i < roots.size(); ++i)
        if (roots[i] == own && i > 0)
            std::swap(roots[0], roots[i]);
    for (const std::string &root : roots)
    {
        if (matches(root + relative))
        {
            *where = root + relative;
            return true;
        }
    }
    // Renamed to another dotted name in the same folder: by its title ID.
    if (copy.image)
        return false;
    for (const std::string &root : roots)
    {
        DIR *dir = ::opendir(real(root + folder).c_str());
        if (dir == nullptr)
            continue;
        std::string found;
        while (const dirent *item = ::readdir(dir))
        {
            const std::string name = item->d_name;
            if (name.size() < 2 || name[0] != '.' || name == "..")
                continue;
            const std::string candidate = root + (folder == "/" ? "" : folder) + "/" + name;
            if (is_folder(candidate) && title_of(candidate) == copy.title_id)
            {
                found = candidate;
                break;
            }
        }
        ::closedir(dir);
        if (!found.empty())
        {
            *where = found;
            return true;
        }
    }
    return false;
}

Unhide unhide_drive_copy(const std::string &title_id, std::string *why)
{
    HiddenCopy copy;
    if (!hidden_copy(title_id, &copy))
        return Unhide::none;
    std::string where;
    if (find_hidden_copy(copy, &where))
    {
        const std::string back = parent_of(where) + "/" + leaf_of(copy.path);
        std::lock_guard<std::mutex> hold(g_lock);
        load();
        if (there(back))
        {
            // Someone put a copy back by that name already: leave both.
            forget(title_id);
            store();
            return Unhide::none;
        }
        if (::rename(real(where).c_str(), real(back).c_str()) != 0)
        {
            *why = std::strerror(errno);
            return Unhide::failed;
        }
        forget(title_id);
        store();
        return Unhide::done;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    if (its_folder_is_here(copy))
    {
        // Its drive is here and the copy isn't: deleted, or renamed back by hand.
        forget(title_id);
        store();
        return Unhide::none;
    }
    for (HiddenCopy &item : g_hidden)
        if (item.title_id == title_id)
            item.unhide = true;
    store();
    return Unhide::later;
}

int finish_unhides()
{
    int done = 0;
    for (const HiddenCopy &copy : hidden_copies())
    {
        if (!copy.unhide)
            continue;
        std::string why;
        if (unhide_drive_copy(copy.title_id, &why) == Unhide::done)
            ++done;
    }
    prune_hidden();
    return done;
}

void prune_hidden()
{
    for (const HiddenCopy &copy : hidden_copies())
    {
        std::string where;
        if (copy.unhide || find_hidden_copy(copy, &where) || !its_folder_is_here(copy))
            continue;
        std::lock_guard<std::mutex> hold(g_lock);
        forget(copy.title_id);
        store();
    }
}

bool hidden_warning_off()
{
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    return g_warning_off;
}

void set_hidden_warning_off(bool off)
{
    std::lock_guard<std::mutex> hold(g_lock);
    load();
    g_warning_off = off;
    store();
}

} // namespace hui::concepts
