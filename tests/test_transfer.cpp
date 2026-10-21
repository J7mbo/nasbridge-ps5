// NASBridge - Tests for the copy queue and deleting games (transfer.cpp), run
// on a computer against tests/fake_nas.py (a WebDAV NAS and a ShadowMountPlus API).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/copy_helper.hpp"
#include "concepts/delete_helper.hpp"
#include "concepts/exfat_image.hpp"
#include "concepts/notify.hpp"
#include "concepts/smp.hpp"
#include "concepts/transfer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
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
std::mutex g_notes_lock;
std::vector<std::string> g_notes; // PS5 notifications sent

void check(bool ok, const std::string &what, const std::string &detail = "")
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  [",
                detail.empty() ? "" : (detail + "]").c_str());
    if (!ok)
        ++g_failures;
}

// The notifications sent since the last call.
std::vector<std::string> notes()
{
    std::lock_guard<std::mutex> hold(g_notes_lock);
    std::vector<std::string> out;
    out.swap(g_notes);
    return out;
}

bool noted(const std::vector<std::string> &list, const std::string &start)
{
    for (const std::string &note : list)
        if (note.compare(0, start.size(), start) == 0)
            return true;
    return false;
}

std::string joined(const std::vector<std::string> &list)
{
    std::string out;
    for (const std::string &note : list)
        out += note + " | ";
    return out;
}

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

void write_file(const std::string &path, std::size_t size, unsigned seed)
{
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
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

bool same(const std::string &a, const std::string &b)
{
    return std::system(("cmp -s \"" + a + "\" \"" + b + "\"").c_str()) == 0;
}

bool there(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string read_text(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void set_control(const std::string &json)
{
    std::ofstream(g_control) << json;
}

std::string smp_log()
{
    std::ifstream in(g_control + ".log");
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

std::uint64_t now_ms()
{
    timespec at{};
    clock_gettime(CLOCK_MONOTONIC, &at);
    return static_cast<std::uint64_t>(at.tv_sec) * 1000u +
           static_cast<std::uint64_t>(at.tv_nsec / 1000000);
}

bool stopped(const QueueStatus &q)
{
    return !q.running();
}

const char *name(QueueState state)
{
    switch (state)
    {
    case QueueState::idle: return "idle";
    case QueueState::running: return "running";
    case QueueState::paused: return "paused";
    case QueueState::blocked: return "blocked";
    case QueueState::failed: return "failed";
    }
    return "?";
}

std::string order(const QueueStatus &q)
{
    std::string out;
    for (const QueueEntry &item : q.entries)
        out += item.entry + ";";
    return out;
}

NasGame folder_game(const std::string &entry, const std::string &base, std::uint64_t size)
{
    NasGame game;
    game.entry = entry;
    game.base = base;
    game.title_id = "PPSA01111";
    game.size = size;
    return game;
}

// "" when every file under `folder` reads back from the image identical
// (through NASBridge's own exFAT reader), or the first that doesn't.
std::string image_mismatch(const std::string &image, const std::string &folder)
{
    const std::string list = g_work + "/image-files.txt";
    std::system(("cd \"" + folder + "\" && find . -type f | sed 's|^\\./||' > \"" + list + "\"").c_str());
    std::ifstream names(list);
    std::string path;
    int count = 0;
    while (std::getline(names, path))
    {
        ExfatReader reader([&image](std::uint64_t offset, std::size_t length, std::string *out)
                           {
                               std::ifstream in(image, std::ios::binary);
                               out->assign(length, '\0');
                               in.seekg(static_cast<std::streamoff>(offset));
                               in.read(&(*out)[0], static_cast<std::streamsize>(length));
                               return static_cast<std::size_t>(in.gcount()) == length;
                           });
        reader.set_limits(64u << 20, 1 << 20);
        std::string why;
        std::vector<ExfatFile> entries;
        if (!reader.open(&why) || !reader.list_root(&entries, &why))
            return "the image doesn't open: " + why;
        std::size_t start = 0;
        std::string data;
        bool found = false;
        for (;;)
        {
            const std::size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            const ExfatFile *entry = find_entry(entries, part.c_str());
            if (entry == nullptr)
                break;
            if (slash == std::string::npos)
            {
                found = reader.read(*entry, 64u << 20, &data, &why);
                break;
            }
            const ExfatFile inside = *entry;
            if (!reader.list(inside, &entries, &why))
                break;
            start = slash + 1;
        }
        if (!found || data != read_text(folder + "/" + path))
            return path;
        ++count;
    }
    return count == 0 ? "no files" : "";
}

NasGame image_game(std::uint64_t size)
{
    NasGame game;
    game.entry = "Img.exfat";
    game.image = true;
    game.image_path = "Img.exfat";
    game.title_id = "PPSA03333";
    game.size = size;
    return game;
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // each line as it happens
    if (argc < 2)
        return 2;
    g_work = argv[1];
    // argv[4], optional: the part of the suite to run (1, 2 or 3; 0 or none:
    // all of it). `make test` runs the three parts at once, each with its own
    // work folder and ports, so the suite takes as long as its longest part.
    const int part = argc > 4 ? std::atoi(argv[4]) : 0;
    if (part < 0 || part > 3)
        return 2;
    const int ports = 18100 + part * 10; // the NAS, the loaders, the helpers
    const int nas_port = ports, loader_port = ports + 4, helper_port = ports + 5,
              copy_loader_port = ports + 6, copier_port = ports + 7;
    const int smp_port = 10110 + part;
    set_smp_port(smp_port);
    if (g_work[0] != '/')
    {
        // Absolute, as on the PS5: the copy helper only takes absolute paths.
        char cwd[4096];
        if (::getcwd(cwd, sizeof(cwd)) != nullptr)
            g_work = std::string(cwd) + "/" + g_work;
    }
    const std::string nas = g_work + "/nas";
    const std::string dest = g_work + "/dest";
    g_control = g_work + "/control.json";
    std::system(("rm -rf \"" + g_work + "\" && mkdir -p \"" + dest + "\" \"" + g_work +
                 "/app\"").c_str());
    set_transfer_root(g_work + "/app");
    // On the PS5 every file goes through the copy helper payload; the first
    // sections test the in-app copier (its fallback), the last the helper.
    set_copy_helper("", 0, 0);
    // Ships defaulting to one connection (best for a single-HDD NAS). The suite
    // exercises the parallel engine, so ask for four; one test below drops back
    // to one to keep the shipping default covered.
    set_copy_streams(4);
    set_notify_hook(
        [](const std::string &text)
        {
            std::lock_guard<std::mutex> hold(g_notes_lock);
            g_notes.push_back(text);
        });

    // The NAS: a folder game, a game nested a folder down with a patch
    // beside it, a game four folders down, and an image.
    const std::string one = nas + "/games/Game One";
    write_file(one + "/sce_sys/param.json", 900, 1);
    write_file(one + "/data/big.bin", 24u << 20, 2);
    write_file(one + "/data/sub/tiny.txt", 3, 3);
    write_file(one + "/data/sub/empty.txt", 0, 4);
    std::system(("mkdir -p \"" + one + "/data/empty folder\"").c_str());
    write_file(one + "/Game One.zip", 100000, 5); // a download: left out
    write_file(one + "/.DS_Store", 10, 6);        // hidden: left out
    write_file(one + "/@eaDir/thumb", 10, 7);     // NAS-internal: left out
    const std::string two = nas + "/games/Game Two";
    write_file(two + "/PPSA02222-app/sce_sys/param.json", 500, 8);
    write_file(two + "/PPSA02222-app/eboot.bin", 3u << 20, 9);
    write_file(two + "/PPSA02222-patch/eboot.bin", 1000, 10); // not the game folder
    const std::string deep = nas + "/games/Game Deep";
    write_file(deep + "/a/b/c/PPSA05555-app/sce_sys/param.json", 400, 13);
    write_file(deep + "/a/b/c/PPSA05555-app/eboot.bin", 2u << 20, 14);
    write_file(deep + "/a/b/c/PPSA05555-app/data/sub/level.bin", 5000, 15);
    write_file(deep + "/a/b/c/PPSA05555-patch/eboot.bin", 100, 16); // not the game folder
    write_file(deep + "/a/readme.txt", 50, 17);                     // above it: left out
    write_file(nas + "/games/Img.exfat", 6u << 20, 11);
    set_control("{}");
    const std::uint64_t one_size = (24u << 20) + 903;
    const std::uint64_t two_size = (3u << 20) + 500;
    const std::uint64_t img_size = 6u << 20;
    const std::uint64_t deep_size = (2u << 20) + 400 + 5000;

    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", nas.c_str(),
               std::to_string(nas_port).c_str(), std::to_string(smp_port).c_str(),
               g_control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(nas_port);
    wait_for_port(smp_port);

    // A stand-in ELF loader runs the delete helper for the delete test. The
    // host helper binary is argv[2]; without it the delete test is skipped.
    const std::string helper = argc > 2 ? argv[2] : std::string();
    const std::string ps5root = g_work + "/ps5root";
    const std::string loader_control = g_work + "/loader.json";
    std::ofstream(loader_control) << "{}";
    std::system(("mkdir -p \"" + ps5root + "\"").c_str());
    pid_t loader = -1;
    if (!helper.empty())
    {
        loader = fork();
        if (loader == 0)
        {
            execlp("python3", "python3", "tests/fake_loader.py",
                   std::to_string(loader_port).c_str(), helper.c_str(),
                   std::to_string(helper_port).c_str(),
                   ps5root.c_str(), loader_control.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        wait_for_port(loader_port);
        set_delete_helper(helper, loader_port, helper_port);
    }

    Connection connection;
    connection.host = "127.0.0.1";
    connection.port = nas_port;
    connection.folder = "/games/";
    std::string why;
    // The drive's free space, as the queue asks before each game.
    std::atomic<std::uint64_t> free_space{1ull << 40};
    const auto space = [&free_space](const std::string &, std::uint64_t *free)
    {
        *free = free_space.load();
        return true;
    };
    const auto clean = [&](const std::string &name)
    { std::system(("rm -rf \"" + dest + "/" + name + "\"").c_str()); };

    // Part 1 (1 to 9).
    if (part == 0 || part == 1)
    {
        // 1. One game, straight through.
        {
            Transfers transfers;
            transfers.set_space_check(space);
            check(transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                                    "Game One", dest, &why),
                  "added to the queue, and it starts", why);
            wait_for(transfers, stopped);
            const QueueStatus q = transfers.queue();
            check(q.state == QueueState::idle && q.entries.empty(), "the queue empties when done",
                  name(q.state));
            const std::string got = dest + "/Game One";
            check(same(one + "/data/big.bin", got + "/data/big.bin") &&
                      same(one + "/sce_sys/param.json", got + "/sce_sys/param.json"),
                  "files identical");
            check(same(one + "/data/sub/tiny.txt", got + "/data/sub/tiny.txt") &&
                      there(got + "/data/sub/empty.txt") && there(got + "/data/empty folder"),
                  "small and empty files and an empty folder there");
            check(!there(got + "/Game One.zip") && !there(got + "/.DS_Store") && !there(got + "/@eaDir"),
                  "archives, hidden and NAS-internal files left out");
            check(!there(dest + "/.nasbridge-incoming") && !there(g_work + "/app/queue.txt"),
                  "staging folder and saved queue gone");
            check(smp_log().find("/api/v1/scan {\"reset_attempts\": true}") != std::string::npos,
                  "ShadowMountPlus asked to rescan");
            check(q.finished_entry == "Game One" && q.finished_destination == dest,
                  "the finished game is remembered for its page");
            const std::vector<std::string> sent = notes();
            check(sent.size() == 1 && noted(sent, "Game One is copied to " + dest +
                                                      ". The game will be on your home screen shortly."),
                  "a finished copy is announced once", joined(sent));
        }

        // 1b. ShadowMountPlus takes the scan but holds it while NASBridge is open
        //     (it counts NASBridge as a running game): the page and the
        //     notification say to close NASBridge.
        {
            clean("Game One");
            set_control("{\"scan_deferred\": \"game_active\"}");
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            wait_for(transfers, stopped);
            const QueueStatus q = transfers.queue();
            check(q.waiting("Game One") != nullptr && q.waiting("Game One")->destination == dest,
                  "a held-back scan marks the game as waiting for NASBridge to close");
            const std::vector<std::string> sent = notes();
            check(noted(sent, "Game One is copied to " + dest +
                                  ". Close NASBridge to add the game to your home screen."),
                  "the notification says to close NASBridge", joined(sent));
            set_control("{}");
        }

        // 2. The same name again: it stops, and removing it leaves the other alone.
        {
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            wait_for(transfers, stopped);
            QueueStatus q = transfers.queue();
            check(q.state == QueueState::failed && q.message.find("already") != std::string::npos,
                  "won't copy over a game of the same name", q.message);
            check(noted(notes(), "Copying Game One stopped: There's already"),
                  "a copy that stops is announced");
            check(transfers.remove(0, &why), "a stopped game can be removed", why);
            wait_for(transfers, [](const QueueStatus &s) { return s.discarding.empty(); });
            q = transfers.queue();
            check(q.state == QueueState::idle && same(one + "/data/big.bin", dest + "/Game One/data/big.bin"),
                  "removing it left the existing game alone", name(q.state));
            clean("Game One");
        }

        // 1c. A NAS whose deep listing stays silent: the game is listed folder
        //     by folder soon after, and that NAS isn't asked deeply again.
        {
            clean("Game One");
            set_deep_listing_wait(1);
            set_control("{\"deep_silent_s\": 20, \"folder_delay_ms\": 150}");
            std::remove((g_control + ".nas.log").c_str());
            const auto deep_asks = []
            {
                const std::string log = read_text(g_control + ".nas.log");
                int count = 0;
                for (std::size_t at = log.find("PROPFIND infinity"); at != std::string::npos;
                     at = log.find("PROPFIND infinity", at + 1))
                    ++count;
                return count;
            };
            for (int round = 0; round < 2; ++round)
            {
                const std::uint64_t began = now_ms();
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                                  "Game One", dest, &why);
                bool counted = false;
                wait_for(transfers, [&](const QueueStatus &s)
                         {
                             if (s.detail.find(" found") != std::string::npos)
                                 counted = true;
                             return stopped(s);
                         });
                const std::uint64_t took = now_ms() - began;
                check(transfers.queue().state == QueueState::idle &&
                          same(one + "/data/big.bin", dest + "/Game One/data/big.bin") && took < 10000,
                      round == 0 ? "a silent deep listing falls back to folder by folder quickly"
                                 : "and the second game copies too",
                      std::to_string(took) + " ms");
                if (round == 0)
                    check(counted, "listing shows how many files it has found");
                clean("Game One");
                notes();
            }
            check(deep_asks() == 1, "a NAS that didn't list deeply isn't asked deeply again",
                  std::to_string(deep_asks()));
            set_deep_listing_wait(8);
            set_control("{}");
        }

        // 3. Three games: order, moving, pausing and resuming.
        {
            set_control("{\"throttle\": 6000000}");
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            transfers.enqueue(connection, folder_game("Game Two", "Game Two/PPSA02222-app/sce_sys", two_size),
                              "Game Two", dest, &why);
            transfers.enqueue(connection, image_game(img_size), "Img", dest, &why);
            QueueStatus q = transfers.queue();
            check(order(q) == "Game One;Game Two;Img.exfat;" && q.running(), "queued in order",
                  order(q));
            check(!transfers.enqueue(connection, image_game(img_size), "Img", dest, &why),
                  "a game can't be queued twice", why);
            check(q.queued_for(dest) <= one_size + two_size + img_size &&
                      q.queued_for(dest) > two_size + img_size,
                  "the queue adds up what it will still write");
            wait_for(transfers, [](const QueueStatus &s)
                     { return !s.entries.empty() && s.entries[0].done > (4u << 20); });
            check(!transfers.move(0, 1, &why), "the game being copied stays first", why);
            check(!transfers.move(1, -1, &why), "nothing jumps ahead of the game being copied", why);
            check(transfers.move(1, 1, &why) && order(transfers.queue()) == "Game One;Img.exfat;Game Two;",
                  "waiting games move", order(transfers.queue()));
            check(!transfers.remove(0, &why), "the game being copied can't be removed", why);
            check(transfers.pause(), "pause accepted");
            wait_for(transfers, stopped);
            q = transfers.queue();
            const std::uint64_t paused_at = q.entries[0].done;
            check(q.state == QueueState::paused, "pauses", name(q.state));
            check(there(g_work + "/app/queue.txt"), "the queue is saved while paused");
            check(!there(dest + "/Game One"), "nothing visible to ShadowMountPlus while paused");
            sleep_ms(400);
            check(transfers.queue().entries[0].done == paused_at, "nothing moves while paused");
            check(notes().empty(), "pausing isn't announced");
            check(transfers.move(0, 1, &why) && order(transfers.queue()) == "Img.exfat;Game One;Game Two;",
                  "paused, the front game can move too", order(transfers.queue()));
            set_control("{}");
            check(transfers.resume(connection, &why), "resume accepted", why);
            wait_for(transfers, stopped);
            q = transfers.queue();
            const std::vector<std::string> sent = notes();
            check(q.state == QueueState::idle && sent.size() == 3 && noted({sent[0]}, "Img is copied") &&
                      noted({sent[1]}, "Game One is copied") && noted({sent[2]}, "Game Two is copied"),
                  "all three copied, in the new order", joined(sent));
            check(same(one + "/data/big.bin", dest + "/Game One/data/big.bin") &&
                      same(two + "/PPSA02222-app/eboot.bin", dest + "/Game Two/eboot.bin") &&
                      !there(dest + "/Game Two/PPSA02222-patch") &&
                      same(nas + "/games/Img.exfat", dest + "/Img.exfat"),
                  "every game identical (a nested game copies its game folder; an image is one file)");
            clean("Game One");
            clean("Game Two");
            clean("Img.exfat");
        }

        // 4. Paused, NASBridge closes, opens again: the queue comes back paused.
        {
            set_control("{\"throttle\": 6000000}");
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                                  "Game One", dest, &why);
                transfers.enqueue(connection, image_game(img_size), "Img", dest, &why);
                wait_for(transfers, [](const QueueStatus &s)
                         { return !s.entries.empty() && s.entries[0].done > (6u << 20); });
                transfers.pause();
                wait_for(transfers, stopped);
            } // NASBridge closes
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.load();
            QueueStatus q = transfers.queue();
            check(q.state == QueueState::paused && order(q) == "Game One;Img.exfat;" &&
                      q.entries[0].started && q.entries[0].done > (6u << 20) &&
                      q.message == "Stopped when NASBridge closed",
                  "the queue comes back paused, with its progress", order(q) + " " + name(q.state));
            Connection other = connection;
            other.host = "127.0.0.2";
            check(!transfers.resume(other, &why), "won't resume from another NAS", why);
            set_control("{}");
            check(transfers.resume(connection, &why), "resumes after a restart", why);
            wait_for(transfers, stopped);
            check(transfers.queue().state == QueueState::idle &&
                      same(one + "/data/big.bin", dest + "/Game One/data/big.bin") &&
                      same(nas + "/games/Img.exfat", dest + "/Img.exfat"),
                  "both finish after a restart, identical");
            notes();
            clean("Game One");
            clean("Img.exfat");
        }

        // 5. The connection drops mid-file; the NAS ignores ranges.
        {
            set_control("{\"drop_after\": 5000000, \"throttle\": 20000000}");
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            wait_for(transfers, stopped);
            check(transfers.queue().state == QueueState::idle &&
                      same(one + "/data/big.bin", dest + "/Game One/data/big.bin"),
                  "carries on after the connection drops", transfers.queue().message);
            clean("Game One");
            set_control("{\"throttle\": 8000000}");
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            wait_for(transfers, [](const QueueStatus &s)
                     { return !s.entries.empty() && s.entries[0].done > (8u << 20); });
            transfers.pause();
            wait_for(transfers, stopped);
            set_control("{\"ignore_range\": true}");
            transfers.resume(connection, &why);
            wait_for(transfers, stopped);
            check(transfers.queue().state == QueueState::idle &&
                      same(one + "/data/big.bin", dest + "/Game One/data/big.bin"),
                  "a NAS that ignores ranges still gives an identical copy");
            notes();
            clean("Game One");
            set_control("{}");
        }

        // 6. Removing: a waiting game, and a paused one with files on the drive.
        {
            set_control("{\"throttle\": 6000000}");
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            transfers.enqueue(connection, image_game(img_size), "Img", dest, &why);
            wait_for(transfers, [](const QueueStatus &s)
                     { return !s.entries.empty() && s.entries[0].done > (4u << 20); });
            check(transfers.remove(1, &why) && order(transfers.queue()) == "Game One;" &&
                      transfers.queue().running(),
                  "a waiting game is removed while the queue runs", why);
            transfers.pause();
            wait_for(transfers, stopped);
            check(there(dest + "/.nasbridge-incoming/Game One/data/big.bin"),
                  "the paused game has files on the drive");
            check(transfers.remove(0, &why), "the paused game can be removed", why);
            wait_for(transfers, [](const QueueStatus &s) { return s.discarding.empty(); });
            check(transfers.queue().state == QueueState::idle && !there(dest + "/.nasbridge-incoming") &&
                      !there(dest + "/Game One") && !there(g_work + "/app/queue.txt"),
                  "its copied files and folders are deleted, and the queue is empty");
            set_control("{}");
            notes();
        }

        // 7. Not enough space: checked before each game.
        {
            Transfers transfers;
            transfers.set_space_check(space);
            free_space.store(10u << 20); // 10 MB: Game One doesn't fit
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            wait_for(transfers, stopped);
            QueueStatus q = transfers.queue();
            check(q.state == QueueState::blocked && q.message.find("Not enough space") == 0 &&
                      !there(dest + "/.nasbridge-incoming"),
                  "a game that doesn't fit isn't started", q.message);
            check(noted(notes(), "The copy queue stopped. Not enough space"),
                  "a full drive is announced");
            free_space.store(1ull << 40);
            transfers.resume(connection, &why);
            wait_for(transfers, stopped);
            check(transfers.queue().state == QueueState::idle && there(dest + "/Game One/data/big.bin"),
                  "it copies once there's room");
            clean("Game One");
            notes();

            // The first fits, the second doesn't once the first has used the room.
            std::atomic<int> asked{0};
            transfers.set_space_check(
                [&asked](const std::string &, std::uint64_t *free)
                {
                    *free = asked.fetch_add(1) == 0 ? (40u << 20) : (2u << 20);
                    return true;
                });
            transfers.enqueue(connection, image_game(img_size), "Img", dest, &why);
            transfers.enqueue(connection, folder_game("Game Two", "Game Two/PPSA02222-app/sce_sys", two_size),
                              "Game Two", dest, &why);
            wait_for(transfers, stopped);
            q = transfers.queue();
            check(q.state == QueueState::blocked && order(q) == "Game Two;" &&
                      there(dest + "/Img.exfat") && !there(dest + "/Game Two"),
                  "space is asked again before the next game", order(q) + " " + q.message);
            check(transfers.remove(0, &why) && transfers.queue().state == QueueState::idle,
                  "removing the game that didn't fit empties the queue", why);
            clean("Img.exfat");
            notes();
        }

        // 8. Killed right after a game was moved into place: the next run
        //    only finishes up.
        {
            CopyPlan plan;
            plan.entry = "Game Two";
            plan.title = "Game Two";
            plan.source = "/games/Game Two/PPSA02222-app/";
            plan.destination = dest;
            plan.name = "Game Two";
            plan.nas = nas_identity(connection);
            plan.listed = true;
            plan.moving = true;
            plan.files = {CopyFile{"eboot.bin", 3u << 20}};
            plan.total = 3u << 20;
            std::system(("mkdir -p \"" + dest + "/Game Two\"").c_str());
            std::system(("cp \"" + two + "/PPSA02222-app/eboot.bin\" \"" + dest + "/Game Two/\"").c_str());
            std::ofstream(g_work + "/app/queue.txt") << queue_to_text({{plan, plan.total, plan.total, true}});
            std::remove((g_control + ".log").c_str());
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.load();
            transfers.resume(connection, &why);
            wait_for(transfers, stopped);
            check(transfers.queue().state == QueueState::idle && !there(g_work + "/app/queue.txt") &&
                      smp_log().find("/api/v1/scan") != std::string::npos,
                  "a copy killed after its move only finishes up", name(transfers.queue().state));
            clean("Game Two");
            notes();
        }

        // 9. Deleting a game on the PS5 through the delete helper, also while the
        //    queue copies. ShadowMountPlus is only read (its game list); the delete
        //    goes to the helper, so it works while NASBridge is open. The helper's
        //    own checks are covered thoroughly in test_delete.
        if (!helper.empty())
        {
            const std::string game_dir = ps5root + "/mnt/ext0/games/Game One";
            const auto set_loader = [&](const std::string &json)
            { std::ofstream(loader_control, std::ios::trunc) << json; };
            const auto install_game = [&]
            {
                std::system(("rm -rf \"" + game_dir + "\" && mkdir -p \"" + game_dir +
                             "/sce_sys\"").c_str());
                std::ofstream(game_dir + "/sce_sys/param.json") << "{\"titleId\": \"PPSA01111\"}";
                write_file(game_dir + "/game.bin", 300000, 21);
                for (int k = 0; k < 40; ++k) // enough for several deletes at once
                    write_file(game_dir + "/data/d" + std::to_string(k % 5) + "/f" +
                                   std::to_string(k) + ".bin",
                               static_cast<std::size_t>(100 + k * 37), 50 + static_cast<unsigned>(k));
                std::remove((ps5root + "/uninstalled.txt").c_str());
            };
            // Its game list, with the game either idle (deletable) or running.
            const auto smp_list = [](bool mounted)
            {
                return std::string("\"games\": [{\"title_id\": \"PPSA01111\", \"path\": "
                                   "\"/mnt/ext0/games/Game One\", \"source_type\": \"folder\", "
                                   "\"installed\": true, \"mounted\": ") +
                       (mounted ? "true" : "false") +
                       ", \"can_uninstall\": true, \"can_manage_source\": true}]";
            };
            // The same game as ShadowMountPlus lists an image game (it deletes
            // those itself; a folder game's files go to the helper, see below).
            const auto smp_image = [&smp_list](bool mounted)
            {
                std::string list = smp_list(mounted);
                const std::string folder = "\"source_type\": \"folder\"";
                list.replace(list.find(folder), folder.size(), "\"source_type\": \"image\"");
                return list;
            };
            set_loader("{}");
            install_game();
            set_control("{\"throttle\": 1000000, " + smp_list(false) + "}");
            Transfers transfers;
            transfers.set_space_check(space);
            transfers.enqueue(connection, image_game(img_size), "Img", dest, &why);
            check(transfers.remove_installed("PPSA01111", "Game One", "Game One", &why),
                  "delete from PS5 starts while the queue copies", why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted, "delete from PS5 finishes",
                  transfers.removal().message);
            check(!there(game_dir) &&
                      read_text(ps5root + "/uninstalled.txt").find("PPSA01111") != std::string::npos,
                  "the game's files are gone and it's off the home screen");
            check(transfers.queue().running(), "the queue kept copying");
            transfers.pause();
            wait_for(transfers, stopped);
            transfers.remove(0, &why);
            wait_for(transfers, [](const QueueStatus &s) { return s.discarding.empty(); });
            check(noted(notes(), "Game One was deleted from the PS5."), "a finished delete is announced");

            // Copied again after that delete: its page mustn't still say "Deleted".
            transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                              "Game One", dest, &why);
            check(transfers.removal().state == TransferState::idle,
                  "copying a deleted game again clears its \"Deleted\"",
                  std::to_string(static_cast<int>(transfers.removal().state)));
            transfers.pause();
            wait_for(transfers, stopped);
            transfers.remove(0, &why);
            wait_for(transfers, [](const QueueStatus &s) { return s.discarding.empty(); });
            clean("Game One");
            notes();

            // An attached game, and a ShadowMountPlus older than 1.7beta6 that
            // won't change games while NASBridge is open: the helper can't
            // release it, so it isn't sent, and NASBridge says what helps.
            install_game();
            set_control("{" + smp_list(true) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::failed &&
                      transfers.removal().message.find("older than 1.7beta6-fix2, update it") !=
                          std::string::npos &&
                      there(game_dir),
                  "an attached game isn't deleted by an older ShadowMountPlus, and it says why",
                  transfers.removal().message);

            // The system uninstall is refused: the files still go, but NASBridge
            // says the home-screen entry is still there.
            install_game();
            set_control("{" + smp_list(false) + "}");
            set_loader("{\"uninstall_fails\": true}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::failed &&
                      transfers.removal().message.find("home screen") != std::string::npos &&
                      !there(game_dir),
                  "a refused uninstall deletes the files but keeps the tile, and says so",
                  transfers.removal().message);
            check(noted(notes(), "Couldn't delete Game One: "), "a failed delete is announced");
            set_loader("{}");
            set_control("{}");

            // ShadowMountPlus 1.7beta6 and newer, NASBridge being a homebrew app
            // (a LAPY title ID): it changes other games while NASBridge is open.
            // An attached folder game (as one just added is): ShadowMountPlus
            // takes it off the home screen and the helper deletes its files.
            install_game();
            std::remove((g_control + ".log").c_str());
            std::remove((ps5root + "/uninstalled.txt").c_str());
            set_control("{\"homebrew_allowed\": true, " + smp_list(true) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(game_dir) &&
                      smp_log().find("/api/v1/games/uninstall") != std::string::npos &&
                      read_text(ps5root + "/uninstalled.txt").find("PPSA01111") ==
                          std::string::npos,
                  "with ShadowMountPlus 1.7beta6, an attached folder game is deleted while "
                  "NASBridge is open",
                  transfers.removal().message);
            notes();
            set_control("{}");

            // The same ShadowMountPlus, from here on: for an image game it
            // deletes and uninstalls, no helper (a folder game is below). (This
            // stand-in deletes nothing itself, so the files still being there
            // shows the helper wasn't sent.)
            const std::string caps = "\"homebrew_allowed\": true";
            install_game();
            std::remove((g_control + ".log").c_str());
            set_control("{" + caps + ", " + smp_image(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && there(game_dir) &&
                      smp_log().find("/api/v1/games/delete {\"confirm\": true, \"title_id\": \"PPSA01111\"}") != std::string::npos &&
                      smp_log().find("/api/v1/games/uninstall {\"title_id\": \"PPSA01111\"}") != std::string::npos,
                  "ShadowMountPlus 1.7beta6 deletes and uninstalls an image game itself",
                  transfers.removal().message);
            notes();

            // A game whose files are attached ("mounted": any image game that has
            // been played) is ShadowMountPlus's to release: NASBridge doesn't
            // refuse it itself.
            install_game();
            set_control("{" + caps + ", " + smp_image(true) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && there(game_dir),
                  "an attached game is deleted by ShadowMountPlus, not refused by NASBridge",
                  transfers.removal().message);
            notes();

            // It refuses an attached game (one is running): the helper isn't
            // sent (it can't release it), and NASBridge says why.
            install_game();
            set_control("{" + caps + ", \"delete_request_refused\": \"game_active\", " +
                        smp_image(true) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::failed && there(game_dir) &&
                      read_text(ps5root + "/uninstalled.txt").empty() &&
                      transfers.removal().message.find("won't change this game while NASBridge "
                                                       "is open") != std::string::npos,
                  "when ShadowMountPlus refuses an attached game, nothing else deletes it",
                  transfers.removal().message);
            notes();

            // ShadowMountPlus is busy scanning right after the delete: the
            // uninstall waits for it.
            install_game();
            set_control("{" + caps + ", \"uninstall_busy\": 2, " + smp_image(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted,
                  "an uninstall waits while ShadowMountPlus is busy scanning",
                  transfers.removal().message);
            notes();

            // The uninstall is refused (not busy) but the game is already off
            // the home screen: done.
            install_game();
            set_control("{" + caps + ", \"uninstall_refused\": \"not_installed\", " +
                        "\"games\": [{\"title_id\": \"PPSA01111\", \"path\": "
                        "\"/mnt/ext0/games/Game One\", \"source_type\": \"image\", "
                        "\"installed\": false}]}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted,
                  "a game already off the home screen counts as uninstalled",
                  transfers.removal().message);
            notes();

            // The uninstall is refused after the files went: the reason, plainly.
            install_game();
            set_control("{" + caps + ", \"uninstall_refused\": \"title_unmount_busy\", " +
                        smp_image(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().message ==
                      "The game files are deleted, but removing the game from the home screen failed: the "
                      "PS5 still has the game's files open. Try again in a moment. If it keeps happening, "
                      "restart the PS5 and try again.",
                  "a refused uninstall says why in plain words", transfers.removal().message);
            notes();

            // It refuses the delete up front: the helper does it instead.
            set_control("{" + caps + ", \"delete_request_refused\": \"game_active\", " +
                        smp_image(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(game_dir) &&
                      read_text(ps5root + "/uninstalled.txt").find("PPSA01111") != std::string::npos,
                  "when ShadowMountPlus refuses up front, the helper deletes it",
                  transfers.removal().message);
            notes();

            // It refuses inside its job, before deleting anything: the helper again.
            install_game();
            set_control("{" + caps + ", \"delete_refused\": \"game_active\", " + smp_image(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(game_dir),
                  "when ShadowMountPlus refuses inside its job, the helper deletes it",
                  transfers.removal().message);
            notes();

            // A folder game: ShadowMountPlus takes it off the home screen, then
            // the helper moves it into .nasbridge-deleting (out of
            // ShadowMountPlus's sight) and deletes it there several files at once
            // (ShadowMountPlus's own delete goes one at a time: minutes for tens
            // of thousands of files), and ShadowMountPlus is asked to scan so its
            // list drops the game.
            const std::string hidden = ps5root + "/mnt/ext0/games/.nasbridge-deleting";
            set_delete_threads(4);
            install_game();
            std::remove((g_control + ".log").c_str());
            set_control("{" + caps + ", " + smp_list(false) + "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(game_dir) &&
                      !transfers.removal().leftover &&
                      smp_log().find("/api/v1/games/uninstall {\"title_id\": \"PPSA01111\"}") != std::string::npos &&
                      smp_log().find("/api/v1/games/delete") == std::string::npos &&
                      read_text(ps5root + "/uninstalled.txt").find("PPSA01111") == std::string::npos &&
                      smp_log().find("/api/v1/scan") != std::string::npos && !there(hidden),
                  "a folder game: ShadowMountPlus uninstalls, the helper deletes its files, a scan "
                  "follows", transfers.removal().message + " | " + smp_log());
            notes();

            // The helper stops partway: it's off the home screen with files left,
            // hidden in .nasbridge-deleting where ShadowMountPlus never looks.
            // NASBridge says so and offers to finish.
            install_game();
            set_control("{" + caps + ", " + smp_list(false) + "}");
            set_loader("{\"delete_fails\": \"game.bin\"}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::failed && transfers.removal().leftover &&
                      !there(game_dir) && there(hidden + "/Game One/game.bin") &&
                      transfers.removal().message.find("Delete the rest") != std::string::npos,
                  "a folder delete that stops leaves its files hidden, and says so",
                  transfers.removal().message);
            notes();
            // Delete the rest: ShadowMountPlus has dropped it from its list by now.
            set_loader("{}");
            set_control("{" + caps + ", \"games\": []}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(hidden) &&
                      !transfers.removal().leftover,
                  "Delete the rest finishes a folder delete that stopped", transfers.removal().message);
            notes();

            // Cut short and NASBridge closed: the next start finishes it quietly.
            install_game();
            set_control("{" + caps + ", " + smp_list(false) + "}");
            set_loader("{\"delete_fails\": \"game.bin\"}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            set_loader("{}");
            {
                Transfers next_start;
                next_start.load();
                for (int i = 0; i < 200 && there(hidden); ++i)
                    sleep_ms(50);
                check(!there(hidden) && !there(g_work + "/app/deleting.txt"),
                      "a delete cut short is finished at the next start");
            }
            notes();

            // ShadowMountPlus won't take it off the home screen (a game is
            // running): nothing has changed, so the helper does it all, as before.
            install_game();
            set_control("{" + caps + ", \"uninstall_refused\": \"game_active\", " + smp_list(false) +
                        "}");
            transfers.remove_installed("PPSA01111", "Game One", "Game One", &why);
            wait_for(transfers, [&](const QueueStatus &)
                     { return transfers.removal().state != TransferState::deleting; });
            check(transfers.removal().state == TransferState::deleted && !there(game_dir) &&
                      read_text(ps5root + "/uninstalled.txt").find("PPSA01111") != std::string::npos,
                  "when ShadowMountPlus won't uninstall a folder game, the helper does it all",
                  transfers.removal().message);
            notes();
            set_delete_threads(16);

            // With ShadowMountPlus 1.7beta6, a copy asks for one scan, which runs
            // while NASBridge is open, so it isn't held back for NASBridge to close.
            std::remove((g_control + ".log").c_str());
            set_control("{" + caps + "}");
            {
                Transfers copying;
                copying.set_space_check(space);
                copying.enqueue(connection, image_game(img_size), "Img", dest, &why);
                wait_for(copying, stopped);
                const std::string log = smp_log();
                const std::string ask = "/api/v1/scan {\"reset_attempts\": true}";
                const std::size_t first = log.find(ask);
                check(copying.queue().waiting("Img.exfat") == nullptr && first != std::string::npos &&
                          log.find(ask, first + 1) == std::string::npos,
                      "a copy asks ShadowMountPlus for one scan, and isn't held back", log);
            }
            clean("Img.exfat");
            notes();
            set_control("{}");
        }
    }

    // Part 2 (10 and 12).
    if (part == 0 || part == 2)
    {
        // 10. A big file comes down over several connections at once.
        {
            const std::string big = nas + "/games/Big.exfat";
            write_file(big, 100u << 20, 12); // 100 MB: four pieces at a time
            NasGame image;
            image.entry = "Big.exfat";
            image.image = true;
            image.image_path = "Big.exfat";
            image.title_id = "PPSA04444";
            image.size = 100u << 20;

            // Each connection is held to 10 MB/s, as one Wi-Fi connection might be.
            set_control("{\"throttle\": 10000000}");
            Transfers transfers;
            transfers.set_space_check(space);
            const auto started = std::chrono::steady_clock::now();
            transfers.enqueue(connection, image, "Big", dest, &why);
            int most = 0; // connections seen bringing data in at once
            wait_for(transfers, [&most](const QueueStatus &q)
                     {
                         most = std::max(most, q.connections);
                         return stopped(q);
                     }, 120);
            check(most == 4, "four connections bring data in at once", std::to_string(most));
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            check(transfers.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                  "a big file arrives identical", transfers.queue().message);
            char speed[64];
            std::snprintf(speed, sizeof(speed), "%.1f s, %.0f MB/s", seconds, 100.0 * 1.048576 / seconds);
            // One connection at 10 MB/s would take over 10 s.
            check(seconds < 6.0, "several connections beat one connection's limit", speed);
            check(!there(dest + "/.nasbridge-incoming") && !there(g_work + "/app/part.txt"),
                  "no pieces or records left behind");
            clean("Big.exfat");
            notes();

            // The shipping default: one connection. Same file, still identical, and
            // the panel reports exactly one connection (not four).
            set_copy_streams(1);
            {
                set_control("{\"throttle\": 40000000}"); // slow enough to see the panel
                Transfers single;
                single.set_space_check(space);
                single.enqueue(connection, image, "Big", dest, &why);
                int most = 0;
                wait_for(single, [&most](const QueueStatus &q)
                         {
                             most = std::max(most, q.connections);
                             return stopped(q);
                         }, 120);
                check(most == 1, "one connection when that's the setting", std::to_string(most));
                check(single.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                      "a big file arrives identical over one connection", single.queue().message);
                clean("Big.exfat");
                notes();
            }
            // One connection, paused part-way and resumed after a restart: the
            // file grows as it's written (not made full size first), and carries
            // on from what's in.
            {
                set_control("{\"throttle\": 12000000}");
                {
                    Transfers first;
                    first.set_space_check(space);
                    first.enqueue(connection, image, "Big", dest, &why);
                    wait_for(first, [](const QueueStatus &q)
                             { return !q.entries.empty() && q.entries[0].done > (12u << 20); });
                    first.pause();
                    wait_for(first, stopped);
                }
                const std::string part = dest + "/.nasbridge-incoming/Big.exfat.nbpart";
                struct stat st{};
                const bool grown = ::stat(part.c_str(), &st) == 0 && st.st_size > 0 &&
                                   static_cast<std::uint64_t>(st.st_size) < (100u << 20);
                check(grown, "one connection grows the file instead of sizing it up front",
                      std::to_string(static_cast<long long>(st.st_size)));
                set_control("{}");
                Transfers again;
                again.set_space_check(space);
                again.load();
                again.resume(connection, &why);
                wait_for(again, stopped, 120);
                check(again.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                      "one connection resumed after a restart, identical", again.queue().message);
                clean("Big.exfat");
                notes();
            }
            set_copy_streams(4);

            // Paused part-way, NASBridge closes, opens again: the pieces in are kept.
            set_control("{\"throttle\": 4000000}");
            {
                Transfers first;
                first.set_space_check(space);
                first.enqueue(connection, image, "Big", dest, &why);
                wait_for(first, [](const QueueStatus &q)
                         { return !q.entries.empty() && q.entries[0].done > (40u << 20); });
                first.pause();
                wait_for(first, stopped);
            }
            check(there(dest + "/.nasbridge-incoming/Big.exfat.nbpart") &&
                      !there(dest + "/.nasbridge-incoming/Big.exfat") && there(g_work + "/app/part.txt"),
                  "while paused, the file is still a part file, with its record");
            std::string record;
            {
                std::ifstream in(g_work + "/app/part.txt");
                std::getline(in, record);
                std::getline(in, record);
            }
            const std::size_t in_already = static_cast<std::size_t>(
                std::count(record.begin(), record.end(), '1'));
            check(in_already >= 1, "the record holds the pieces already in", record.substr(record.rfind('\t') + 1));
            set_control("{}");
            Transfers again;
            again.set_space_check(space);
            again.load();
            again.resume(connection, &why);
            wait_for(again, stopped, 120);
            check(again.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                  "resumed after a restart, identical", again.queue().message);
            clean("Big.exfat");
            notes();

            // A connection drops mid-piece: that piece is fetched again.
            set_control("{\"drop_after\": 3000000}");
            Transfers dropping;
            dropping.set_space_check(space);
            dropping.enqueue(connection, image, "Big", dest, &why);
            wait_for(dropping, stopped, 120);
            check(dropping.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                  "a dropped piece is fetched again", dropping.queue().message);
            clean("Big.exfat");
            notes();

            // Mid-file, the NAS answers one piece with the whole file: a hiccup,
            // retried. The copy stays in pieces (one connection would take 10 s).
            set_control("{\"throttle\": 10000000, \"stray_after\": 4}");
            {
                Transfers stray;
                stray.set_space_check(space);
                const auto began = std::chrono::steady_clock::now();
                stray.enqueue(connection, image, "Big", dest, &why);
                wait_for(stray, stopped, 120);
                const double took =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                char text[32];
                std::snprintf(text, sizeof(text), "%.1f s", took);
                check(stray.queue().state == QueueState::idle && same(big, dest + "/Big.exfat") &&
                          took < 8.0,
                      "a stray whole-file answer mid-copy doesn't drop to one connection", text);
            }
            clean("Big.exfat");
            notes();

            // The NAS is busy (503) for a while: the connections wait and rejoin.
            set_control("{\"busy\": 4}"); // each connection is turned away once
            {
                Transfers busy;
                busy.set_space_check(space);
                busy.enqueue(connection, image, "Big", dest, &why);
                wait_for(busy, stopped, 120);
                check(busy.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                      "busy answers are waited out, and the copy finishes",
                      busy.queue().message);
            }
            clean("Big.exfat");
            notes();

            // Pause lands quickly even while the NAS is silent between bursts
            // (here 64 KB, then three seconds of nothing).
            set_control("{\"throttle\": 20000}");
            {
                Transfers quiet;
                quiet.set_space_check(space);
                quiet.enqueue(connection, image, "Big", dest, &why);
                wait_for(quiet, [](const QueueStatus &q)
                         { return !q.entries.empty() && q.entries[0].done > 0; });
                sleep_ms(400);
                const auto asked = std::chrono::steady_clock::now();
                quiet.pause();
                wait_for(quiet, stopped, 20);
                const double took =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
                char text[32];
                std::snprintf(text, sizeof(text), "%.2f s", took);
                check(quiet.queue().state == QueueState::paused && took < 1.0,
                      "Pause lands within a second, even while the NAS is quiet", text);
                std::string ignored;
                quiet.remove(0, &ignored);
                wait_for(quiet, [](const QueueStatus &q) { return q.discarding.empty() && q.entries.empty(); });
            }
            clean("Big.exfat");
            notes();

            // A NAS that ignores ranges: the file comes in one go instead.
            set_control("{\"ignore_range\": true}");
            Transfers whole;
            whole.set_space_check(space);
            whole.enqueue(connection, image, "Big", dest, &why);
            wait_for(whole, stopped, 120);
            check(whole.queue().state == QueueState::idle && same(big, dest + "/Big.exfat") &&
                      !there(dest + "/.nasbridge-incoming"),
                  "a NAS without ranges still gives an identical copy", whole.queue().message);
            clean("Big.exfat");
            notes();

            // A big file half written in one go (by an older NASBridge, or after
            // the NAS stopped sending pieces): its start is kept, the rest comes
            // in pieces.
            for (const bool ranges : {true, false})
            {
                const std::uint64_t had = 50u << 20; // what the old copy had written
                std::system(("mkdir -p \"" + dest + "/.nasbridge-incoming\"").c_str());
                std::system(("head -c " + std::to_string(had) + " \"" + big + "\" > \"" + dest +
                             "/.nasbridge-incoming/Big.exfat\"").c_str());
                CopyPlan plan;
                plan.entry = "Big.exfat";
                plan.title = "Big";
                plan.title_id = "PPSA04444";
                plan.image = true;
                plan.source = "/games/Big.exfat";
                plan.destination = dest;
                plan.name = "Big.exfat";
                plan.nas = nas_identity(connection);
                plan.listed = true;
                plan.files = {CopyFile{"", 100u << 20}};
                plan.total = 100u << 20;
                std::ofstream(g_work + "/app/queue.txt") << queue_to_text({{plan, plan.total, had, true}});
                set_control(ranges ? "{\"throttle\": 10000000, \"ignore_range\": false}"
                                   : "{\"ignore_range\": true}");
                Transfers old;
                old.set_space_check(space);
                old.load();
                check(old.queue().state == QueueState::paused && old.queue().entries.size() == 1,
                      "it comes back as a paused queue");
                const auto started = std::chrono::steady_clock::now();
                old.resume(connection, &why);
                bool pieces_seen = false;
                for (int i = 0; i < 2400 && old.queue().running(); ++i)
                {
                    pieces_seen = pieces_seen || there(g_work + "/app/part.txt");
                    sleep_ms(50);
                }
                const double seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                check(old.queue().state == QueueState::idle && same(big, dest + "/Big.exfat"),
                      ranges ? "the old half-written copy finishes identical, in pieces"
                             : "without ranges, the old copy still finishes identical",
                      old.queue().message);
                if (ranges)
                {
                    char speed[48];
                    std::snprintf(speed, sizeof(speed), "%.1f s for the last 68 MB", seconds);
                    // The first whole piece (32 MB) is kept; 68 MB are left, which
                    // one connection at 10 MB/s needs 6.8 s for.
                    check(pieces_seen && seconds < 5.0, "the rest came over several connections", speed);
                }
                clean("Big.exfat");
                notes();
            }

            // Removed while paused: the part file and its record go too.
            set_control("{\"throttle\": 4000000}");
            Transfers removing;
            removing.set_space_check(space);
            removing.enqueue(connection, image, "Big", dest, &why);
            wait_for(removing, [](const QueueStatus &q)
                     { return !q.entries.empty() && q.entries[0].done > (20u << 20); });
            removing.pause();
            wait_for(removing, stopped);
            removing.remove(0, &why);
            wait_for(removing, [](const QueueStatus &q) { return q.discarding.empty(); });
            check(!there(dest + "/.nasbridge-incoming") && !there(g_work + "/app/part.txt"),
                  "removing it deletes the part file and its record");
            set_control("{}");
            notes();
        }

        // 12. A game four folders down (Game Deep/a/b/c/PPSA05555-app/sce_sys):
        //     the folder holding sce_sys becomes the game's folder on the PS5.
        {
            clean("Game Deep");
            set_control("{}");
            Transfers transfers;
            transfers.set_space_check(space);
            const NasGame game =
                folder_game("Game Deep", "Game Deep/a/b/c/PPSA05555-app/sce_sys", deep_size);
            check(transfers.enqueue(connection, game, "Game Deep", dest, &why),
                  "a game four folders down is queued", why);
            wait_for(transfers, stopped, 60);
            const std::string app = deep + "/a/b/c/PPSA05555-app";
            const std::string got = dest + "/Game Deep";
            check(transfers.queue().state == QueueState::idle &&
                      same(app + "/sce_sys/param.json", got + "/sce_sys/param.json") &&
                      same(app + "/eboot.bin", got + "/eboot.bin") &&
                      same(app + "/data/sub/level.bin", got + "/data/sub/level.bin"),
                  "a game four folders down copies with sce_sys at the top of its folder",
                  transfers.queue().message);
            check(!there(got + "/a") && !there(got + "/c") && !there(got + "/PPSA05555-app") &&
                      !there(got + "/PPSA05555-patch") && !there(got + "/readme.txt"),
                  "the folders above it, and the patch beside it, are left out");
            clean("Game Deep");
            notes();
        }
    }

    // Part 3 (11).
    pid_t copy_loader = -1;
    if (part == 0 || part == 3)
    {
        // 11. Through the copy helper payload, as on the PS5: the stand-in loader
        //     runs the host-built helper (argv[3]); without it this is skipped.
        const std::string copier = argc > 3 ? argv[3] : std::string();
        if (!copier.empty())
        {
            ::setenv("NB_TEST_IDLE", "5", 1); // the helper leaves soon after the tests
            copy_loader = fork();
            if (copy_loader == 0)
            {
                execlp("python3", "python3", "tests/fake_loader.py",
                           std::to_string(copy_loader_port).c_str(), copier.c_str(),
                           std::to_string(copier_port).c_str(),
                       ps5root.c_str(), loader_control.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
            wait_for_port(copy_loader_port);
            set_copy_helper(copier, copy_loader_port, copier_port);
            set_copy_streams(1);

            // a. A folder game: every file through the helper, identical.
            clean("Game One");
            {
                set_control("{\"throttle\": 20000000}");
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, folder_game("Game One", "Game One/sce_sys", one_size),
                                  "Game One", dest, &why);
                bool via_helper = false;
                wait_for(transfers, [&via_helper](const QueueStatus &q)
                         {
                             if (q.conn_report.find("helper") != std::string::npos)
                                 via_helper = true;
                             return stopped(q);
                         }, 120);
                const QueueStatus q = transfers.queue();
                const std::string got = dest + "/Game One";
                check(q.state == QueueState::idle && same(one + "/data/big.bin", got + "/data/big.bin") &&
                          same(one + "/sce_sys/param.json", got + "/sce_sys/param.json") &&
                          same(one + "/data/sub/tiny.txt", got + "/data/sub/tiny.txt") &&
                          there(got + "/data/sub/empty.txt"),
                      "a folder game copies identical through the copy helper", q.message);
                check(via_helper, "the copy helper did the copying (the panel says \"helper\")");
                clean("Game One");
                set_control("{}");
                notes();
            }

            // a2. Many small files (a folder game can be tens of thousands): one
            //     batch over a few kept-open connections, not one per file, with
            //     a big file among them on its own; identical.
            const std::string many = nas + "/games/Many Files";
            std::uint64_t many_size = 0;
            int many_files = 0;
            write_file(many + "/sce_sys/param.json", 700, 30);
            many_size += 700;
            ++many_files;
            for (int folder = 0; folder < 12; ++folder)
                for (int k = 0; k < 50; ++k)
                {
                    const std::uint64_t size = static_cast<std::uint64_t>((folder * 50 + k) * 37 % 9000);
                    write_file(many + "/data/f" + std::to_string(folder) + "/file" + std::to_string(k) +
                                   ".bin",
                               size, 31 + folder * 50 + k);
                    many_size += size;
                    ++many_files;
                }
            write_file(many + "/data/f5/zz big.bin", 20u << 20, 40);
            many_size += 20u << 20;
            ++many_files;
            const auto same_tree = [&](const std::string &got)
            {
                return std::system(("diff -r \"" + many + "\" \"" + got + "\" >/dev/null").c_str()) == 0;
            };
            const auto connections = [&]
            {
                const std::string log = read_text(g_control + ".conn.log");
                return static_cast<int>(std::count(log.begin(), log.end(), '\n'));
            };
            const NasGame many_game = folder_game("Many Files", "Many Files/sce_sys", many_size);
            clean("Many Files");
            std::remove((g_control + ".conn.log").c_str());
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, many_game, "Many Files", dest, &why);
                bool batched = false;
                int most = 0;
                wait_for(transfers, [&](const QueueStatus &q)
                         {
                             if (q.conn_report.find(" at once ") != std::string::npos &&
                                 q.conn_report.find("files/s") != std::string::npos)
                                 batched = true;
                             most = std::max(most, q.files_done);
                             return stopped(q);
                         }, 120);
                check(transfers.queue().state == QueueState::idle && same_tree(dest + "/Many Files"),
                      "many small files copy identical in batches", transfers.queue().message);
                check(batched, "the panel says how many files come in at once");
                // 16 workers, each with one connection, plus listing: far fewer than
                // one a file.
                check(connections() < 60, "a batch keeps its connections to the NAS open",
                      std::to_string(connections()) + " connections for " +
                          std::to_string(many_files) + " files");
            }
            clean("Many Files");
            notes();

            // a3. Paused mid-batch and NASBridge opened again: carries on, identical.
            set_control("{\"throttle\": 400000}");
            {
                Transfers first;
                first.set_space_check(space);
                first.enqueue(connection, many_game, "Many Files", dest, &why);
                wait_for(first, [](const QueueStatus &q) { return q.files_done > 100; }, 60);
                first.pause();
                wait_for(first, stopped);
            }
            set_control("{}");
            {
                Transfers again;
                again.set_space_check(space);
                again.load();
                again.resume(connection, &why);
                wait_for(again, stopped, 120);
                check(again.queue().state == QueueState::idle && same_tree(dest + "/Many Files"),
                      "a paused batch resumes after a restart, identical", again.queue().message);
            }
            clean("Many Files");
            notes();

            // a4. The NAS drops a connection mid-batch: tried again, identical.
            set_control("{\"drop_get\": 150}");
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, many_game, "Many Files", dest, &why);
                wait_for(transfers, stopped, 120);
                check(transfers.queue().state == QueueState::idle && same_tree(dest + "/Many Files"),
                      "a connection dropped mid-batch is made again, identical",
                      transfers.queue().message);
            }
            set_control("{}");
            clean("Many Files");
            notes();

            // a5. An older helper, without batches: one file at a time, identical.
            //     A game of its own, small: each file on its own costs the helper's
            //     quarter-second progress tick, which 600 files made 2.5 minutes.
            const std::string few = nas + "/games/Few Files";
            std::uint64_t few_size = 700;
            write_file(few + "/sce_sys/param.json", 700, 60);
            for (int k = 0; k < 24; ++k)
            {
                const std::uint64_t size = static_cast<std::uint64_t>(k * 311 % 5000);
                write_file(few + "/data/f" + std::to_string(k % 3) + "/file" + std::to_string(k) + ".bin",
                           size, 61 + static_cast<unsigned>(k));
                few_size += size;
            }
            std::ofstream(ps5root + "/no_batch") << "1";
            {
                Transfers transfers;
                transfers.set_space_check(space);
                const std::uint64_t began = now_ms();
                transfers.enqueue(connection, folder_game("Few Files", "Few Files/sce_sys", few_size),
                                  "Few Files", dest, &why);
                wait_for(transfers, stopped, 120);
                check(transfers.queue().state == QueueState::idle &&
                          std::system(("diff -r \"" + few + "\" \"" + dest + "/Few Files\" >/dev/null")
                                          .c_str()) == 0,
                      "an older copy helper copies them one at a time, identical",
                      std::string(name(transfers.queue().state)) + " " + transfers.queue().message + " after " +
                          std::to_string((now_ms() - began) / 1000) + " s, " +
                          std::to_string(transfers.queue().files_done) + " files");
            }
            std::remove((ps5root + "/no_batch").c_str());
            clean("Few Files");
            notes();

            // b. A big image paused part-way, NASBridge closed and opened again:
            //    the pause lands at once, and the copy carries on, identical.
            write_file(nas + "/games/Copy.exfat", 64u << 20, 21);
            NasGame big = image_game(64u << 20);
            big.entry = "Copy.exfat";
            big.image_path = "Copy.exfat";
            big.title_id = "PPSA05555";
            const std::string staged = dest + "/.nasbridge-incoming/Copy.exfat";
            set_control("{\"throttle\": 8000000}");
            {
                Transfers first;
                first.set_space_check(space);
                first.enqueue(connection, big, "Copy", dest, &why);
                bool two = false;
                wait_for(first, [&two](const QueueStatus &q)
                         {
                             if (q.conn_report.find("helper x2") != std::string::npos && q.connections == 2)
                                 two = true;
                             return !q.entries.empty() && q.entries[0].done > (16u << 20) && two;
                         });
                check(two, "a big file comes in over two connections (\"helper x2\")");
                const auto asked = std::chrono::steady_clock::now();
                first.pause();
                wait_for(first, stopped);
                const double took =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
                char text[32];
                std::snprintf(text, sizeof(text), "%.2f s", took);
                check(took < 1.0, "Pause lands within a second through the copy helper", text);
            }
            struct stat st{};
            check(::stat(staged.c_str(), &st) == 0 && st.st_size > 0 &&
                      static_cast<std::uint64_t>(st.st_size) < (64u << 20),
                  "while paused, the helper's file holds what came in",
                  std::to_string(static_cast<long long>(st.st_size)));
            set_control("{}");
            {
                Transfers again;
                again.set_space_check(space);
                again.load();
                again.resume(connection, &why);
                wait_for(again, stopped, 120);
                check(again.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "resumed through the copy helper after a restart, identical", again.queue().message);
            }
            clean("Copy.exfat");
            notes();

            // c. The NAS drops a connection mid-file: tried again, identical. Over
            //    two connections the chunk is asked for again inside the helper;
            //    over one, NASBridge starts the helper again from where it got.
            set_control("{\"drop_after\": 8000000}");
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, big, "Copy", dest, &why);
                wait_for(transfers, stopped, 120);
                check(transfers.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "a dropped chunk is fetched again over two connections", transfers.queue().message);
            }
            clean("Copy.exfat");
            notes();
            set_helper_streams(1);
            set_control("{\"drop_after\": 20000000}");
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, big, "Copy", dest, &why);
                wait_for(transfers, stopped, 120);
                check(transfers.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "a dropped connection is tried again over one connection", transfers.queue().message);
            }
            clean("Copy.exfat");
            set_control("{}");
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, big, "Copy", dest, &why);
                bool one = true;
                wait_for(transfers, [&one](const QueueStatus &q)
                         {
                             if (q.conn_report.find("x2") != std::string::npos)
                                 one = false;
                             return stopped(q);
                         }, 120);
                check(one && transfers.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "one connection when that's the helper's setting, identical", transfers.queue().message);
            }
            clean("Copy.exfat");
            notes();
            set_helper_streams(2);

            // d. Resumed from a NAS that ignores ranges: the whole file again.
            set_control("{\"throttle\": 8000000}");
            {
                Transfers first;
                first.set_space_check(space);
                first.enqueue(connection, big, "Copy", dest, &why);
                wait_for(first, [](const QueueStatus &q)
                         { return !q.entries.empty() && q.entries[0].done > (16u << 20); });
                first.pause();
                wait_for(first, stopped);
            }
            set_control("{\"ignore_range\": true}");
            {
                Transfers again;
                again.set_space_check(space);
                again.load();
                again.resume(connection, &why);
                wait_for(again, stopped, 120);
                check(again.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "a NAS that ignores ranges still gives an identical copy through the helper",
                      again.queue().message);
            }
            clean("Copy.exfat");
            set_control("{}");
            notes();

            // d2. Two connections crash the helper (as a too-small thread stack once
            //     did on the PS5): the retry says why, and the copy finishes over one.
            std::ofstream(ps5root + "/crash_multi") << "1";
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, big, "Copy", dest, &why);
                std::string said;
                wait_for(transfers, [&said](const QueueStatus &q)
                         {
                             if (q.detail.find("Connection lost (") != std::string::npos)
                                 said = q.detail;
                             return stopped(q);
                         }, 120);
                check(transfers.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "when two connections fail, the copy finishes over one", transfers.queue().message);
                check(said.find("copy helper stopped answering") != std::string::npos,
                      "the retry says why it failed", said);
            }
            std::remove((ps5root + "/crash_multi").c_str());
            clean("Copy.exfat");
            notes();

            // f. Copy as an exFAT image: a folder game becomes one .exfat file
            //    holding every file, identical. Paused part-way and NASBridge
            //    opened again, it carries on from the image's length.
            set_copy_as_image(true);
            {
                const std::string image = dest + "/Many Files.exfat";
                const std::string staged_image = dest + "/.nasbridge-incoming/Many Files.exfat";
                set_control("{\"throttle\": 1000000}");
                {
                    Transfers first;
                    first.set_space_check(space);
                    first.enqueue(connection, many_game, "Many Files", dest, &why);
                    bool one_file = false;
                    std::uint64_t size = 0;
                    wait_for(first, [&](const QueueStatus &q)
                             {
                                 if (q.entries.empty())
                                     return true;
                                 one_file = q.entries[0].image;
                                 size = q.entries[0].size;
                                 return q.entries[0].done > (6u << 20);
                             }, 60);
                    first.pause();
                    wait_for(first, stopped);
                    // At most: each file's last cluster (64 KiB), the 64 MiB kept
                    // free inside, and the file system's own few MB.
                    const std::uint64_t most = many_size + static_cast<std::uint64_t>(many_files) * 65536 +
                                               (64u << 20) + (8u << 20);
                    check(one_file && size > many_size && size <= most,
                          "a folder game queued as an image is one file, a little bigger",
                          std::to_string(size) + " for " + std::to_string(many_size));
                }
                struct stat part{};
                check(::stat(staged_image.c_str(), &part) == 0 && part.st_size > 0 && !there(dest + "/Many Files"),
                      "while paused, the image is one file in the staging folder");
                set_control("{}");
                {
                    Transfers again;
                    again.set_space_check(space);
                    again.load();
                    again.resume(connection, &why);
                    wait_for(again, stopped, 120);
                    check(again.queue().state == QueueState::idle && there(image) && !there(staged_image),
                          "the image copy resumes after a restart and is moved into place",
                          again.queue().message);
                }
                const std::string mismatch = image_mismatch(image, many);
                check(mismatch.empty(), "every file in the image reads back identical", mismatch);
                if (std::system("command -v fsck.exfat >/dev/null 2>&1") == 0)
                    check(std::system(("fsck.exfat -n \"" + image + "\" >/dev/null 2>&1").c_str()) == 0,
                          "fsck.exfat finds nothing wrong with the image");
                clean("Many Files.exfat");
                notes();

                // Removed from the queue while paused: the half image is deleted.
                set_control("{\"throttle\": 1000000}");
                {
                    Transfers transfers;
                    transfers.set_space_check(space);
                    transfers.enqueue(connection, many_game, "Many Files", dest, &why);
                    wait_for(transfers, [](const QueueStatus &q)
                             { return q.entries.empty() || q.entries[0].done > (2u << 20); }, 60);
                    transfers.pause();
                    wait_for(transfers, stopped);
                    transfers.remove(0, &why);
                    wait_for(transfers, [](const QueueStatus &q) { return q.entries.empty() && q.discarding.empty(); });
                    check(!there(staged_image) && !there(dest + "/.nasbridge-incoming"),
                          "removing a paused image copy deletes the half image");
                }
                set_control("{}");
                notes();
            }
            set_copy_as_image(false);

            // e. The helper can't be started (no loader answers): copied in-app.
            set_copy_helper(copier, 18099, 18098);
            {
                Transfers transfers;
                transfers.set_space_check(space);
                transfers.enqueue(connection, big, "Copy", dest, &why);
                wait_for(transfers, stopped, 120);
                check(transfers.queue().state == QueueState::idle &&
                          same(nas + "/games/Copy.exfat", dest + "/Copy.exfat"),
                      "without the copy helper, it copies in-app instead", transfers.queue().message);
            }
            clean("Copy.exfat");
            notes();
            set_copy_helper("", 0, 0);
        }
    }

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    if (loader > 0)
    {
        kill(loader, SIGTERM);
        waitpid(loader, nullptr, 0);
    }
    if (copy_loader > 0)
    {
        kill(copy_loader, SIGTERM);
        waitpid(copy_loader, nullptr, 0);
    }
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
