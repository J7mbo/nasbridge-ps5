// NASBridge - Tests for a USB drive as the source (drive.cpp, and the copy
// queue reading from one), run on a computer: the drives are folders under
// the work folder (set_drive_prefix), ShadowMountPlus is tests/fake_nas.py's,
// and the copy helper is the host-built one run by tests/fake_loader.py.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/connection.hpp"
#include "concepts/copy_helper.hpp"
#include "concepts/drive.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas_probe.hpp"
#include "concepts/notify.hpp"
#include "concepts/smp.hpp"
#include "concepts/transfer.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace hui::concepts;

namespace
{

int g_failures = 0;
std::string g_work;
std::string g_control;

void check(bool ok, const std::string &what, const std::string &detail = "")
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  [",
                detail.empty() ? "" : (detail + "]").c_str());
    if (!ok)
        ++g_failures;
}

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

void run(const std::string &command)
{
    if (std::system(command.c_str()) != 0)
        std::printf("note: \"%s\" failed\n", command.c_str());
}

void write_file(const std::string &path, std::size_t size, unsigned seed)
{
    run("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"");
    std::ofstream out(path, std::ios::binary);
    unsigned x = seed * 2654435761u + 1;
    std::vector<char> block(65536);
    while (size > 0)
    {
        const std::size_t n = size < block.size() ? size : block.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            x = x * 1103515245u + 12345u;
            block[i] = static_cast<char>(x >> 16);
        }
        out.write(block.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
}

void write_text(const std::string &path, const std::string &text)
{
    run("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"");
    std::ofstream(path) << text;
}

bool same(const std::string &a, const std::string &b)
{
    return std::system(("cmp -s \"" + a + "\" \"" + b + "\"").c_str()) == 0;
}

bool there(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

void set_control(const std::string &json)
{
    std::ofstream(g_control) << json;
}

bool wait_for(Transfers &transfers, const std::function<bool(const QueueStatus &)> &done,
              int seconds = 60)
{
    for (int i = 0; i < seconds * 20; ++i)
    {
        if (done(transfers.queue()))
            return true;
        sleep_ms(50);
    }
    return false;
}

bool stopped(const QueueStatus &q)
{
    return !q.running();
}

NasGame folder_game(const std::string &entry, const std::string &title_id)
{
    NasGame game;
    game.entry = entry;
    game.base = entry + "/sce_sys";
    game.title_id = title_id;
    return game;
}

NasGame image_game(const std::string &file, const std::string &title_id)
{
    NasGame game;
    game.entry = file;
    game.image = true;
    game.image_path = file;
    game.title_id = title_id;
    return game;
}

// ShadowMountPlus's list: one game, from `path`.
std::string smp_has(const std::string &title_id, const std::string &path, bool image)
{
    return "{\"homebrew_allowed\": true, \"games\": [{\"title_id\": \"" + title_id +
           "\", \"path\": \"" + path + "\", \"source_type\": \"" + (image ? "image" : "folder") +
           "\", \"installed\": true, \"mounted\": false, \"can_uninstall\": true, "
           "\"can_manage_source\": true}]}";
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2)
        return 2;
    g_work = argv[1];
    if (g_work[0] != '/')
    {
        char cwd[4096];
        if (::getcwd(cwd, sizeof(cwd)) != nullptr)
            g_work = std::string(cwd) + "/" + g_work;
    }
    const std::string copier = argc > 2 ? argv[2] : std::string();
    const int nas_port = 18160, smp_port = 10130, loader_port = 18164, copier_port = 18165;
    run("rm -rf \"" + g_work + "\" && mkdir -p \"" + g_work + "/app\" \"" + g_work +
        "/dest\" \"" + g_work + "/ps5root\" \"" + g_work + "/nas\"");
    set_drive_prefix(g_work); // /mnt/usb0 is <work>/mnt/usb0
    set_cache_root(g_work + "/app");
    set_transfer_root(g_work + "/app");
    set_smp_port(smp_port);
    set_copy_helper("", 0, 0);
    set_notify_hook([](const std::string &) {});
    g_control = g_work + "/control.json";
    set_control("{}");
    const std::string mnt = g_work + "/mnt";
    const std::string dest = g_work + "/dest";

    // ---- the drives ----
    run("mkdir -p \"" + mnt + "/usb0/Games\"");
    {
        const std::vector<std::string> present = drive_roots_present();
        check(present.size() == 1 && present[0] == "/mnt/usb0", "only a plugged-in drive is listed",
              present.empty() ? "" : present[0]);
        check(drive_root_of("/mnt/usb0/Games/X") == "/mnt/usb0" && drive_root_of("/mnt/usb01").empty() &&
                  drive_root_of("/data/homebrew").empty(),
              "a path's drive is found by its root");
        check(drive_name("/mnt/usb0") == "USB drive 1" && drive_name("/mnt/ext0") == "Extended storage",
              "drives get the names the PS5 uses", drive_name("/mnt/usb0"));
        std::string folder = "/mnt/usb0/Games/";
        check(normalise_drive_folder(&folder).empty() && folder == "/mnt/usb0/Games",
              "a folder on a drive is taken", folder);
        std::string elsewhere = "/data/games";
        check(!normalise_drive_folder(&elsewhere).empty(), "a folder off the drives is refused");
    }

    // ---- reading it ----
    const std::string one = mnt + "/usb0/Games/Game One";
    write_text(one + "/sce_sys/param.json", "{\"titleId\": \"PPSA01111\"}");
    write_file(one + "/data/big.bin", 24u << 20, 2);
    write_file(one + "/data/small.bin", 70000, 3);
    write_file(one + "/data/sub/tiny.txt", 3, 4);
    write_file(one + "/data/sub/empty.txt", 0, 5);
    write_file(one + "/.DS_Store", 10, 6);
    write_file(mnt + "/usb0/Games/Img.exfat", 9u << 20, 7);
    run("mkdir -p \"" + mnt + "/usb0/Games/System Volume Information\" \"" + mnt +
        "/usb0/Games/.hidden\"");
    {
        std::vector<DavEntry> entries;
        int error = 0;
        const bool ok = drive_list("/mnt/usb0/Games", &entries, &error);
        std::string names;
        bool sized = false;
        for (const DavEntry &entry : entries)
        {
            names += entry.name + (entry.folder ? "/;" : ";");
            if (entry.name == "Img.exfat")
                sized = entry.size == (9u << 20) && entry.path == "/mnt/usb0/Games/Img.exfat";
        }
        check(ok && entries.size() == 2 && sized,
              "a listing leaves out hidden and system folders, with sizes and full paths", names);
        std::string bytes;
        check(drive_read_range("/mnt/usb0/Games/Game One/data/sub/tiny.txt", 1, 2, &bytes, &error) &&
                  bytes.size() == 2,
              "a range of a file is read");
        check(!drive_list("/mnt/usb3/Games", &entries, &error) &&
                  drive_error_brief("/mnt/usb3/Games", error) == "the drive isn't connected",
              "a drive that isn't plugged in says so", drive_error_brief("/mnt/usb3/Games", error));
        Connection missing;
        missing.set_type(ConnectionType::drive);
        missing.folder = "/mnt/usb3/Games";
        const ProbeResult said = explain_drive_failure(missing, ENOENT);
        check(said.headline == "USB drive 4 isn't connected", "the test names the missing drive",
              said.headline);
    }

    // ---- hiding a copy, and bringing it back ----
    {
        const std::string game = mnt + "/usb0/homebrew/Astro";
        write_text(game + "/sce_sys/param.json", "{\"titleId\": \"PPSA07777\"}");
        std::string why;
        check(hide_drive_copy("PPSA07777", "/mnt/usb0/homebrew/Astro/", false, &why) &&
                  there(mnt + "/usb0/homebrew/.Astro") && !there(game),
              "a drive copy is hidden behind a dot", why);
        HiddenCopy copy;
        check(hidden_copy("PPSA07777", &copy) && copy.path == "/mnt/usb0/homebrew/Astro" &&
                  copy.hidden == "/mnt/usb0/homebrew/.Astro",
              "the hidden copy is remembered", copy.hidden);
        check(unhide_drive_copy("PPSA07777", &why) == Unhide::done && there(game) &&
                  !hidden_copy("PPSA07777", &copy),
              "its PS5 copy deleted, it gets its name back", why);

        // Unplugged when its PS5 copy goes: done once it's back, in another port.
        hide_drive_copy("PPSA07777", "/mnt/usb0/homebrew/Astro", false, &why);
        run("mv \"" + mnt + "/usb0\" \"" + mnt + "/away\"");
        std::string where;
        check(hidden_copy("PPSA07777", &copy) && !find_hidden_copy(copy, &where),
              "an unplugged drive's hidden copy isn't found (the delete warning's case)");
        check(unhide_drive_copy("PPSA07777", &why) == Unhide::later, "unplugged: it waits");
        check(finish_unhides() == 0 && hidden_copy("PPSA07777", &copy) && copy.unhide,
              "still unplugged: it keeps waiting");
        run("mv \"" + mnt + "/away\" \"" + mnt + "/usb1\"");
        check(finish_unhides() == 1 && there(mnt + "/usb1/homebrew/Astro") &&
                  !hidden_copy("PPSA07777", &copy),
              "plugged into another port, it's renamed back there");

        // Renamed by hand to another dotted name: found by its title ID.
        hide_drive_copy("PPSA07777", "/mnt/usb1/homebrew/Astro", false, &why);
        run("mv \"" + mnt + "/usb1/homebrew/.Astro\" \"" + mnt + "/usb1/homebrew/.Astro old\"");
        hidden_copy("PPSA07777", &copy);
        check(find_hidden_copy(copy, &where) && where == "/mnt/usb1/homebrew/.Astro old",
              "a hidden copy renamed by hand is found by its title ID", where);
        check(unhide_drive_copy("PPSA07777", &why) == Unhide::done && there(mnt + "/usb1/homebrew/Astro"),
              "and renamed back to its own name", why);

        // Renamed back by hand: forgotten.
        hide_drive_copy("PPSA07777", "/mnt/usb1/homebrew/Astro", false, &why);
        run("mv \"" + mnt + "/usb1/homebrew/.Astro\" \"" + mnt + "/usb1/homebrew/Astro\"");
        prune_hidden();
        check(!hidden_copy("PPSA07777", &copy), "a copy renamed back by hand is forgotten");

        check(!hidden_warning_off(), "the delete warning is on at first");
        set_hidden_warning_off(true);
        check(hidden_warning_off(), "\"Don't warn again\" turns it off");
        run("mv \"" + mnt + "/usb1\" \"" + mnt + "/usb0\"");
    }

    // ---- copying from the drive ----
    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", (g_work + "/nas").c_str(),
               std::to_string(nas_port).c_str(), std::to_string(smp_port).c_str(),
               g_control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(nas_port);
    wait_for_port(smp_port);

    Connection connection;
    connection.set_type(ConnectionType::drive);
    connection.folder = "/mnt/usb0/Games";
    std::string why;
    const auto space = [](const std::string &, std::uint64_t *free)
    {
        *free = 1ull << 40;
        return true;
    };
    const auto copied_ok = [&](const std::string &got)
    {
        return same(one + "/data/big.bin", got + "/data/big.bin") &&
               same(one + "/data/small.bin", got + "/data/small.bin") &&
               same(one + "/sce_sys/param.json", got + "/sce_sys/param.json") &&
               same(one + "/data/sub/tiny.txt", got + "/data/sub/tiny.txt") &&
               there(got + "/data/sub/empty.txt") && !there(got + "/.DS_Store");
    };

    // In-app (no copy helper): ShadowMountPlus doesn't run it from the drive,
    // so the drive copy is left as it is.
    {
        Transfers transfers;
        transfers.set_space_check(space);
        check(transfers.enqueue(connection, folder_game("Game One", "PPSA01111"), "Game One", dest, &why),
              "a drive game is queued", why);
        wait_for(transfers, stopped, 120);
        const QueueStatus q = transfers.queue();
        check(q.state == QueueState::idle && copied_ok(dest + "/Game One"),
              "a folder game copies identical from the drive, in-app", q.message);
        check(there(one), "a drive copy ShadowMountPlus doesn't run isn't hidden");
        run("rm -rf \"" + dest + "/Game One\"");
    }

    // In-app, a game ShadowMountPlus runs from the drive: hidden once copied.
    {
        set_control(smp_has("PPSA03333", "/mnt/usb0/Games/Img.exfat", true));
        Transfers transfers;
        transfers.set_space_check(space);
        transfers.enqueue(connection, image_game("Img.exfat", "PPSA03333"), "Img", dest, &why);
        wait_for(transfers, stopped, 120);
        const QueueStatus q = transfers.queue();
        check(q.state == QueueState::idle && same(mnt + "/usb0/Games/.Img.exfat", dest + "/Img.exfat"),
              "an image copies identical from the drive", q.message);
        check(!there(mnt + "/usb0/Games/Img.exfat") && there(mnt + "/usb0/Games/.Img.exfat"),
              "the drive copy ShadowMountPlus ran is hidden behind a dot");

        // Deleting the PS5 copy brings the drive copy back.
        set_control(smp_has("PPSA03333", dest + "/Img.exfat", true));
        check(transfers.remove_installed("PPSA03333", "Img.exfat", "Img", &why),
              "delete from PS5 starts", why);
        for (int i = 0; i < 600 && transfers.removal().state == TransferState::deleting; ++i)
            sleep_ms(50);
        check(transfers.removal().state == TransferState::deleted && there(mnt + "/usb0/Games/Img.exfat") &&
                  !there(mnt + "/usb0/Games/.Img.exfat"),
              "the PS5 copy deleted, the drive copy gets its name back", transfers.removal().message);
        run("rm -f \"" + dest + "/Img.exfat\"");
        set_control("{}");
    }

    // A drive that's gone mid-queue: the failure speaks of the drive.
    {
        Transfers transfers;
        transfers.set_space_check(space);
        NasGame gone = folder_game("Nope", "PPSA09999");
        transfers.enqueue(connection, gone, "Nope", dest, &why);
        wait_for(transfers, stopped, 60);
        const QueueStatus q = transfers.queue();
        check(q.state == QueueState::failed && q.message.find("NAS") == std::string::npos,
              "a game missing from the drive fails without blaming a NAS", q.message);
        transfers.remove(0, &why);
    }

    // Through the copy helper: a big file over two readers, the rest in a batch.
    pid_t loader = -1;
    if (!copier.empty())
    {
        ::setenv("NB_TEST_IDLE", "5", 1);
        const std::string loader_control = g_work + "/loader.json";
        std::ofstream(loader_control) << "{}";
        loader = fork();
        if (loader == 0)
        {
            execlp("python3", "python3", "tests/fake_loader.py", std::to_string(loader_port).c_str(),
                   copier.c_str(), std::to_string(copier_port).c_str(),
                   (g_work + "/ps5root").c_str(), loader_control.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        wait_for_port(loader_port);
        set_copy_helper(copier, loader_port, copier_port);
        set_helper_streams(2);
        Transfers transfers;
        transfers.set_space_check(space);
        transfers.enqueue(connection, folder_game("Game One", "PPSA01111"), "Game One", dest, &why);
        bool via_helper = false;
        wait_for(transfers, [&via_helper](const QueueStatus &q)
                 {
                     if (q.conn_report.find("helper") != std::string::npos)
                         via_helper = true;
                     return stopped(q);
                 }, 120);
        const QueueStatus q = transfers.queue();
        check(q.state == QueueState::idle && copied_ok(dest + "/Game One"),
              "a folder game copies identical from the drive through the copy helper", q.message);
        check(via_helper, "the copy helper did the copying");
        struct stat big{}, small{};
        check(::stat((dest + "/Game One/data/big.bin").c_str(), &big) == 0 &&
                  ::stat((dest + "/Game One/data/small.bin").c_str(), &small) == 0 &&
                  (big.st_mode & 0111) == 0111 && (small.st_mode & 0111) == 0111,
              "files copied from the drive have the execute bit (issue #4)");
        run("rm -rf \"" + dest + "/Game One\"");
        set_copy_helper("", 0, 0);
    }

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    if (loader > 0)
    {
        kill(loader, SIGTERM);
        waitpid(loader, nullptr, 0);
    }
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
