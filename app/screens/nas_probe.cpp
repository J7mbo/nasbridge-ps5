// NASBridge - "Test connection": one listing from the NAS (WebDAV or SMB), off the UI thread.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/nas_probe.hpp"

#include "concepts/drive.hpp"
#include "concepts/http.hpp"
#include "concepts/smb.hpp"

#include <algorithm>
#include <cerrno>
#include <vector>

namespace hui::concepts
{

namespace
{

using State = ProbeResult::State;

ProbeResult failed(std::string headline, std::string detail)
{
    return {State::failed, std::move(headline), std::move(detail)};
}

std::string plural(int count, const char *one, const char *many)
{
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

// One entry of a folder, from either kind of NAS.
struct Listed
{
    std::string name;
    bool folder = false;
};
// Lists one folder ("/games/Game/"); false when it can't be read.
using Lister = std::function<bool(const std::string &path, std::vector<Listed> *entries)>;

template <typename Entry>
std::vector<Listed> listed(const std::vector<Entry> &entries)
{
    std::vector<Listed> out;
    out.reserve(entries.size());
    for (const Entry &entry : entries)
        out.push_back({entry.name, entry.folder});
    return out;
}

// What the games folder holds: folders (possible game folders) and .exfat images.
ProbeResult describe_entries(const Connection &connection, const std::vector<Listed> &entries)
{
    int folders = 0;
    int images = 0;
    int other = 0;
    for (const Listed &entry : entries)
    {
        if (entry.folder)
            ++folders;
        else if (ends_with_nocase(entry.name, ".exfat"))
            ++images;
        else
            ++other;
    }
    const std::string where = connection.folder.empty() ? "/" : connection.folder;
    ProbeResult result;
    result.state = State::ok;
    result.headline = "Connected";
    if (folders == 0 && images == 0)
        result.detail = other == 0 ? where + " is empty. Pick the folder your games are in."
                                   : where + " has no folders or .exfat images, only " +
                                         plural(other, "other file", "other files") +
                                         ". Pick the folder your games are in.";
    else
        result.detail = "Reached " + where + ". Test connection counts the games in it.";
    return result;
}

// What a 207 listing holds.
ProbeResult describe_listing(const Connection &connection, const std::string &body)
{
    std::vector<DavEntry> entries;
    if (!parse_listing(body, folder_path(connection), &entries))
        return failed("The NAS sent an answer NASBridge can't read",
                      "It said the request worked, but listed nothing. Check that this is the "
                      "NAS's WebDAV address.");
    return describe_entries(connection, listed(entries));
}

bool count_in(const std::string &root, const std::vector<Listed> &top, const Lister &list,
              const std::function<bool()> &stop,
              const std::function<void(int done, int total)> &progress, GameCount *count);

// The test over SMB: sign in, then list the shares (no folder set yet) or
// the games folder, and count the games in it.
ProbeResult probe_smb(const Connection &connection, bool count,
                      const std::function<bool()> &stop,
                      const std::function<void(int, int)> &progress)
{
    SmbSession session(connection);
    SmbFailure failure;
    const auto failed_with = [&](const SmbFailure &why)
    {
        ProbeResult result;
        result.state = State::failed;
        describe_smb_failure(connection, why, &result.headline, &result.detail);
        return result;
    };
    if (smb_location(connection.folder).share.empty())
    {
        std::vector<std::string> shares;
        if (!session.list_shares(&shares, &failure))
            return failed_with(failure);
        ProbeResult result;
        result.state = State::ok;
        result.headline = "Connected";
        if (shares.empty())
        {
            result.detail = "Signed in, but this user can't see any shares. Check the user's "
                            "access on the NAS.";
            return result;
        }
        std::string names;
        for (std::size_t i = 0; i < shares.size() && i < 6; ++i)
            names += (i == 0 ? "" : ", ") + shares[i];
        if (shares.size() > 6)
            names += " and " + std::to_string(shares.size() - 6) + " more";
        result.detail = "Signed in. Shares: " + names +
                        ". Pick your games folder with the folder button.";
        return result;
    }
    std::vector<SmbEntry> top;
    if (!session.list(folder_path(connection), &top, &failure))
        return failed_with(failure);
    ProbeResult result = describe_entries(connection, listed(top));
    GameCount games;
    const Lister list = [&](const std::string &path, std::vector<Listed> *entries)
    {
        std::vector<SmbEntry> inside;
        SmbFailure why;
        if (!session.list(path, &inside, &why))
            return false;
        *entries = listed(inside);
        return true;
    };
    if (count && result.state == State::ok && !top.empty() &&
        count_in(folder_path(connection), listed(top), list, stop, progress, &games))
        result.detail = describe_count(connection, games);
    return result;
}

// The test for a USB drive: list the games folder on it and count the games.
ProbeResult probe_drive(const Connection &connection, bool count,
                        const std::function<bool()> &stop,
                        const std::function<void(int, int)> &progress)
{
    std::vector<DavEntry> top;
    int error = 0;
    if (!drive_list(folder_path(connection), &top, &error))
        return explain_drive_failure(connection, error);
    ProbeResult result = describe_entries(connection, listed(top));
    result.headline = "Found the folder";
    GameCount games;
    const Lister list = [&](const std::string &path, std::vector<Listed> *entries)
    {
        std::vector<DavEntry> inside;
        int why = 0;
        if (!drive_list(path, &inside, &why))
            return false;
        *entries = listed(inside);
        return true;
    };
    if (count && result.state == State::ok && !top.empty() &&
        count_in(folder_path(connection), listed(top), list, stop, progress, &games))
        result.detail = describe_count(connection, games);
    return result;
}

// The whole test, start to finish, on the worker thread.
ProbeResult probe(const Connection &connection, bool count,
                  const std::function<bool()> &stop,
                  const std::function<void(int, int)> &progress)
{
    if (connection.on_drive())
        return probe_drive(connection, count, stop, progress);
    if (connection.host.find_first_not_of("0123456789.") != std::string::npos)
        return failed("Use the NAS's IP address",
                      "Names like nas.local can't be looked up from the PS5 yet. Your NAS's IP "
                      "address is in its network settings, or your router's device list.");
    if (connection.type == ConnectionType::smb)
        return probe_smb(connection, count, stop, progress);
    const HttpReply reply =
        http_request(connection, "PROPFIND", folder_path(connection),
                     "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
                     propfind_body());
    if (!reply.connected)
        return interpret_socket_error(connection, reply.error, true);
    if (count && reply.status == 207)
    {
        ProbeResult result = describe_listing(connection, reply.body);
        std::vector<DavEntry> top;
        parse_listing(reply.body, folder_path(connection), &top);
        GameCount games;
        if (result.state == State::ok && !top.empty() &&
            count_games(connection, top, stop, progress, &games))
            result.detail = describe_count(connection, games);
        return result;
    }
    if (reply.header.empty() && reply.body.empty())
        return interpret_socket_error(connection, reply.error != 0 ? reply.error : ECONNRESET,
                                      false);
    return interpret_response(connection, reply.status, reply.header, reply.body);
}

} // namespace

bool count_games(const Connection &connection, const std::vector<DavEntry> &top,
                 const std::function<bool()> &stop,
                 const std::function<void(int done, int total)> &progress, GameCount *count)
{
    const Lister list = [&](const std::string &path, std::vector<Listed> *entries)
    {
        const HttpReply reply = http_request(
            connection, "PROPFIND", path,
            "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n", propfind_body());
        std::vector<DavEntry> inside;
        if (reply.status != 207 || !parse_listing(reply.body, path, &inside))
            return false;
        *entries = listed(inside);
        return true;
    };
    return count_in(folder_path(connection), listed(top), list, stop, progress, count);
}

namespace
{

bool count_in(const std::string &root, const std::vector<Listed> &top, const Lister &list,
              const std::function<bool()> &stop,
              const std::function<void(int done, int total)> &progress, GameCount *count)
{
    // The library's limits (nas_library.cpp: kDepth, kListings).
    constexpr int kDepth = 4;
    constexpr int kListings = 16;
    *count = GameCount{};
    int total = 0;
    for (const Listed &entry : top)
    {
        if (entry.folder)
            ++total;
        else if (ends_with_nocase(entry.name, ".exfat"))
            ++count->image_games;
    }
    int done = 0;
    if (progress)
        progress(done, total);
    for (const Listed &entry : top)
    {
        if (!entry.folder)
            continue;
        if (stop && stop())
            return false;
        // Breadth first, as the library reads a game folder.
        struct Folder
        {
            std::string path;
            int depth;
        };
        std::vector<Folder> queue{{root + entry.name + "/", 0}};
        bool sce_sys = false;
        bool image = false;
        bool unreadable = false;
        int listings = 0;
        for (std::size_t next = 0; next < queue.size() && listings < kListings && !sce_sys;
             ++next)
        {
            const Folder folder = queue[next];
            ++listings;
            std::vector<Listed> inside;
            if (!list(folder.path, &inside))
            {
                unreadable = unreadable || folder.depth == 0;
                continue;
            }
            for (const Listed &item : inside)
            {
                if (!item.folder)
                    image = image || ends_with_nocase(item.name, ".exfat");
                else if (lowered(item.name) == "sce_sys")
                    sce_sys = true;
                else if (folder.depth + 1 <= kDepth)
                    queue.push_back({folder.path + item.name + "/", folder.depth + 1});
            }
        }
        if (unreadable)
            ++count->unreadable;
        else if (sce_sys)
            ++count->folder_games;
        else if (image)
            ++count->image_games;
        else
            ++count->other_folders;
        if (progress)
            progress(++done, total);
    }
    return true;
}

} // namespace

std::string describe_count(const Connection &connection, const GameCount &count)
{
    const std::string where = connection.folder.empty() ? "/" : connection.folder;
    const int games = count.folder_games + count.image_games;
    std::string text;
    if (games == 0)
    {
        text = "No games in " + where + ". Pick the folder your games are in.";
    }
    else
    {
        text = "Found " + plural(games, "game", "games") + ": ";
        if (count.folder_games > 0 && count.image_games > 0)
            text += plural(count.folder_games, "folder game", "folder games") + " and " +
                    plural(count.image_games, ".exfat game", ".exfat games");
        else if (count.folder_games > 0)
            text += count.folder_games == 1 ? "a folder game" : "all folder games";
        else
            text += count.image_games == 1 ? "an .exfat game" : "all .exfat games";
        text += ".";
    }
    if (count.other_folders > 0)
        text += " " + plural(count.other_folders, "folder has", "folders have") +
                " no game inside.";
    if (count.unreadable > 0)
        text += " " + plural(count.unreadable, "folder", "folders") + " couldn't be read.";
    return text;
}

ProbeResult explain_drive_failure(const Connection &connection, int error)
{
    const std::string where = connection.folder.empty() ? "/" : connection.folder;
    const std::string root = drive_root_of(where);
    if (root.empty())
        return failed("Pick a folder on a USB drive",
                      "Choose the folder your games are in with the folder button.");
    if (!drive_root_present(where))
        return failed(drive_name(root) + " isn't connected",
                      "Plug the drive into the PS5. If it is plugged in, use the folder button "
                      "to pick the folder again: it may be in another port now.");
    if (error == ENOENT || error == ENOTDIR)
        return failed("Couldn't find " + where,
                      "The drive is connected, but that folder isn't on it. Pick the folder "
                      "your games are in with the folder button.");
    if (error == EACCES || error == EPERM)
        return failed("NASBridge isn't allowed to read the drive",
                      "The PS5 refused to show NASBridge " + where +
                          ". ShadowMountPlus is what lets apps read USB drives: check it "
                          "is running.");
    return failed("Couldn't read the drive",
                  "Reading " + where + " failed (" + drive_error_brief(where, error) +
                      "). Check the drive in the PS5's storage settings.");
}

ProbeResult interpret_socket_error(const Connection &connection, int error, bool connecting)
{
    const std::string port = std::to_string(connection.effective_port());
    if (connecting)
    {
        switch (error)
        {
        case ECONNREFUSED:
            return failed("Nothing is answering on port " + port,
                          "The NAS is there, but not on this port. Turn on WebDAV in the NAS's "
                          "settings and check its port (Synology: 5005).");
        case ETIMEDOUT:
            return failed("No answer from " + connection.host,
                          "Check the address, and that the NAS is on and on the same network as "
                          "this PS5.");
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EHOSTDOWN:
        case ENETDOWN:
            return failed("This PS5 can't reach " + connection.host,
                          "Check the address, and that the PS5 is connected to your network.");
        default:
            return failed("Couldn't connect to " + connection.host,
                          "The connection failed (error " + std::to_string(error) +
                              "). Check the address and port.");
        }
    }
    if (error == ETIMEDOUT)
        return failed("The NAS stopped answering",
                      "It accepted the connection but sent nothing back. Check the port: if it's "
                      "an HTTPS port, use the NAS's HTTP WebDAV port instead.");
    return failed("The NAS closed the connection",
                  "If this port uses HTTPS, use the NAS's HTTP WebDAV port instead (Synology: "
                  "5005).");
}

ProbeResult interpret_response(const Connection &connection, int status, const std::string &header,
                               const std::string &body)
{
    const std::string where = connection.folder.empty() ? "/" : connection.folder;
    if (status == 207)
        return describe_listing(connection, body);
    if (status == 0)
        return failed("That port doesn't speak WebDAV",
                      "Something answered, but not a web server. If it's an HTTPS port, use the "
                      "NAS's HTTP WebDAV port instead (Synology: 5005).");
    if (status == 401)
    {
        const std::string offer = lowered(header_value(header, "WWW-Authenticate"));
        if (!offer.empty() && offer.find("basic") == std::string::npos)
            return failed("This sign-in type isn't supported yet",
                          "The NAS only offers Digest sign-in. Allow Basic authentication for "
                          "WebDAV in its settings if you can.");
        if (connection.username.empty())
            return failed("The NAS wants a user name and password",
                          "Enter an account that can read " + where + ".");
        return failed("Wrong user name or password",
                      "The NAS turned down \"" + connection.username + "\". Check both, and that "
                      "the account may use WebDAV.");
    }
    if (status == 403)
        return failed("Access denied",
                      "This account can't read " + where + ". Check its permissions on the NAS.");
    if (status == 404)
        return failed("Folder not found",
                      where + " isn't on the NAS. Folder names must match exactly, capitals "
                              "included.");
    if (status == 405 || status == 501)
        return failed("WebDAV isn't on at this port",
                      "A web server answered, but it doesn't accept WebDAV. Turn on WebDAV in the "
                      "NAS's settings, or check the port (Synology: 5005).");
    if (status >= 300 && status < 400)
    {
        const std::string location = header_value(header, "Location");
        if (lowered(location).compare(0, 8, "https://") == 0)
            return failed("The NAS wants HTTPS",
                          "HTTPS isn't supported yet. Turn on HTTP for WebDAV (Synology: port "
                          "5005) and use that port.");
        return failed("The NAS sent NASBridge elsewhere",
                      location.empty() ? "Check the address and folder."
                                       : "It pointed to " + location +
                                             ". Use that address and folder here.");
    }
    if (status == 400 && lowered(body).find("https") != std::string::npos)
        return failed("That port expects HTTPS",
                      "HTTPS isn't supported yet. Use the NAS's HTTP WebDAV port instead "
                      "(Synology: 5005).");
    if (status >= 200 && status < 300)
        return failed("That's a web server, but not WebDAV",
                      "It ignored the WebDAV request. Check the port (Synology's WebDAV Server "
                      "uses 5005).");
    return failed("The NAS answered with error " + std::to_string(status),
                  "Check the address, port and folder, and that WebDAV is on.");
}

// ---- the worker ----

NasProbe::~NasProbe()
{
    stop_.store(true);
    if (have_thread_)
        pthread_join(thread_, nullptr);
}

void NasProbe::start(const Connection &connection, bool count_games)
{
    if (running())
    {
        // A test of older settings is still out: this one starts after it
        // (a count in progress stops early).
        stop_.store(true);
        pending_ = connection;
        pending_count_ = count_games;
        has_pending_ = true;
        return;
    }
    if (have_thread_)
    {
        pthread_join(thread_, nullptr); // finished: reap it
        have_thread_ = false;
    }
    connection_ = connection;
    count_ = count_games;
    counted_.store(0);
    to_count_.store(0);
    stop_.store(false);
    running_generation_ = generation_.load();
    state_.store(static_cast<int>(State::running));

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    pthread_t thread{};
    const int created = pthread_create(&thread, &attributes, &NasProbe::thread_main, this);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        finished_ = failed("Couldn't start the test", "Try again in a moment.");
        state_.store(static_cast<int>(State::failed));
        return;
    }
    thread_ = thread;
    have_thread_ = true;
}

void NasProbe::poll()
{
    if (has_pending_ && !running())
    {
        has_pending_ = false;
        start(pending_, pending_count_);
    }
}

void NasProbe::reset()
{
    generation_.fetch_add(1);
    stop_.store(true); // a count of the old settings is no use
    if (!running())
        state_.store(static_cast<int>(State::idle));
}

ProbeResult NasProbe::result() const
{
    const State state = static_cast<State>(state_.load());
    if (has_pending_)
    {
        ProbeResult waiting;
        waiting.state = State::running;
        return waiting;
    }
    if (generation_.load() != running_generation_)
        return {}; // the settings changed since this test started
    if (state == State::ok || state == State::failed)
        return finished_;
    ProbeResult result;
    result.state = state;
    return result;
}

void *NasProbe::thread_main(void *self)
{
    static_cast<NasProbe *>(self)->run();
    return nullptr;
}

void NasProbe::run()
{
    ProbeResult result = probe(
        connection_, count_, [this]() { return stop_.load(); },
        [this](int done, int total)
        {
            to_count_.store(total);
            counted_.store(done);
        });
    finished_ = std::move(result);
    state_.store(static_cast<int>(finished_.state)); // publishes finished_
}

} // namespace hui::concepts
