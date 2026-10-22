// NASBridge - The library, the connection test and the copy queue over NFS,
// against a real NFS server (tests/nfs_server.py): what the library finds,
// and games copied in-app and through the copy helper, byte for byte.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/copy_helper.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas.hpp"
#include "concepts/nas_library.hpp"
#include "concepts/nas_probe.hpp"
#include "concepts/nfs.hpp"
#include "concepts/smp.hpp"
#include "concepts/transfer.hpp"

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

void file(const std::string &path, const std::string &text = "x")
{
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
    std::ofstream(path, std::ios::binary) << text;
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

const char *kParam = R"({"titleId":"PPSA00001","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"T"}}})";
const char kPng[] = "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x04"
                    "\x00\x00\x00\x04\x08\x06\x00\x00\x00\xa9\xf1\x9e\x7e\x00\x00\x00\x12\x49\x44\x41"
                    "\x54\x78\x9c\x63\x38\xa1\xa1\xf1\x1f\x19\x33\x90\x2e\x00\x00\x6c\x38\x21\x71\x4b"
                    "\x5e\xd7\x1c\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";

struct Library
{
    bool listed = false;
    bool finished = false;
    std::vector<NasGame> games;
    std::string headline;
    int covers = 0;
    std::vector<std::pair<std::string, std::uint64_t>> sizes;
};

// The library as the screen reads it: what the cache saved last time first.
Library read_library(const Connection &connection)
{
    std::vector<NasGame> saved;
    load_library(connection, &saved);
    LibraryLoader loader;
    loader.start(connection, saved);
    Library got;
    for (int i = 0; i < 1200 && !got.finished; ++i)
    {
        LibraryEvent event;
        while (loader.take(&event))
        {
            switch (event.kind)
            {
            case LibraryEvent::Kind::listed:
                got.listed = true;
                got.games = event.games;
                break;
            case LibraryEvent::Kind::failed:
                got.headline = event.headline;
                break;
            case LibraryEvent::Kind::cover:
                ++got.covers;
                break;
            case LibraryEvent::Kind::size:
                got.sizes.emplace_back(event.entry, event.bytes);
                break;
            case LibraryEvent::Kind::finished:
                got.finished = true;
                break;
            default:
                break;
            }
        }
        sleep_ms(25);
    }
    return got;
}

const NasGame *find(const std::vector<NasGame> &games, const std::string &entry)
{
    for (const NasGame &game : games)
        if (game.entry == entry)
            return &game;
    return nullptr;
}

bool copy(Transfers &transfers, const Connection &connection, const NasGame &game,
          const std::string &dest, std::string *message)
{
    std::string why;
    if (!transfers.enqueue(connection, game, game.entry, dest, &why))
    {
        *message = why;
        return false;
    }
    for (int i = 0; i < 1200; ++i)
    {
        const QueueStatus q = transfers.queue();
        if (!q.running())
        {
            *message = q.message;
            return q.state == QueueState::idle;
        }
        sleep_ms(50);
    }
    *message = "timed out";
    return false;
}

ProbeResult probe(const Connection &connection)
{
    NasProbe probe;
    probe.start(connection, true);
    for (int i = 0; i < 1200 && probe.running(); ++i)
        sleep_ms(25);
    return probe.result();
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2)
        return 2;
    std::string work = argv[1];
    if (work[0] != '/')
    {
        char cwd[4096];
        if (::getcwd(cwd, sizeof(cwd)) != nullptr)
            work = std::string(cwd) + "/" + work;
    }
    const std::string copier = argc > 2 ? argv[2] : std::string();
    const int nfs_port = 18170, mount_port = 18171, smp_web_port = 18172, smp_port = 10130,
              loader_port = 18174, copier_port = 18175, nothing_port = 18176;
    std::system(("rm -rf \"" + work + "\" && mkdir -p \"" + work + "/dest\" \"" + work + "/app\"")
                    .c_str());

    // The export holds the games folder PS5: every layout the library knows,
    // and two games to copy. The NAS path is the export's real path, as on a
    // NAS (/mnt/user/Games/PS5 on Unraid).
    const std::string share = work + "/share";
    const std::string g = share + "/PS5";
    file(g + "/A/sce_sys/param.json", kParam);           // folder game
    file(g + "/A/sce_sys/icon0.png", std::string(kPng, sizeof(kPng) - 1));
    file(g + "/B/PPSA-app/sce_sys/param.json", kParam);  // folder game, a level down
    file(g + "/C/C.exfat");                              // image in a folder
    file(g + "/D/sub/D.exfat");                          // image two levels down
    file(g + "/E.exfat");                                // image in the games folder
    file(g + "/F/F.zip");                                // a download: no game
    file(g + "/G/G.exfat");                              // both: sce_sys wins
    file(g + "/G/inner/sce_sys/param.json", kParam);
    file(g + "/H/x/y/z/sce_sys/param.json", kParam);     // three folders down
    file(g + "/I/a/b/c/d/sce_sys/param.json", kParam);   // four: as deep as is searched
    file(g + "/J/a/b/c/d/e/sce_sys/param.json", kParam); // deeper: not found
    file(g + "/notes.txt");                              // a file: not a game
    file(g + "/@eaDir/sce_sys/param.json", kParam);      // NAS-internal: never a game
    const std::string one = g + "/Game One";
    write_file(one + "/sce_sys/param.json", 900, 1);
    write_file(one + "/data/big.bin", 24u << 20, 2);
    write_file(one + "/data/sub/tiny.txt", 3, 3);
    write_file(one + "/data/sub/empty.txt", 0, 4);
    std::system(("mkdir -p \"" + one + "/data/empty folder\"").c_str());
    write_file(one + "/Game One.zip", 100000, 5); // a download: left out
    write_file(one + "/.DS_Store", 10, 6);        // hidden: left out
    for (int i = 0; i < 40; ++i)                  // a run of small files: a batch
        write_file(one + "/data/many/f" + std::to_string(i) + ".bin", 1000 + i * 37, 20 + i);
    write_file(g + "/Img.exfat", 6u << 20, 11);
    const std::uint64_t one_size = (24u << 20) + 903 + [] {
        std::uint64_t total = 0;
        for (int i = 0; i < 40; ++i)
            total += 1000 + i * 37;
        return total;
    }();
    // Exported to another address only: this "PS5" isn't let in.
    const std::string closed = work + "/closed";
    file(closed + "/PS5/A/sce_sys/param.json", kParam);
    std::system(("chmod -R a+rX \"" + share + "\" \"" + closed + "\"").c_str());

    const pid_t unfsd = fork();
    if (unfsd == 0)
    {
        execlp("python3", "python3", "tests/nfs_server.py", (work + "/unfs").c_str(),
               std::to_string(nfs_port).c_str(), std::to_string(mount_port).c_str(), share.c_str(),
               closed.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    // ShadowMountPlus's API (the fake NAS's other half), asked to rescan after a copy.
    const std::string control = work + "/control.json";
    file(control, "{}");
    std::system(("mkdir -p \"" + work + "/unused\"").c_str());
    const pid_t smp = fork();
    if (smp == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", (work + "/unused").c_str(),
               std::to_string(smp_web_port).c_str(), std::to_string(smp_port).c_str(),
               control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    set_smp_port(smp_port);
    set_nfs_mount_port(mount_port);
    const bool up = wait_for_port(nfs_port) && wait_for_port(mount_port) && wait_for_port(smp_port);
    check(up, "the NFS server is up");

    Connection connection;
    connection.type = ConnectionType::nfs;
    connection.host = "127.0.0.1";
    connection.port = nfs_port;
    connection.folder = g;

    // 1. The exports, and the folders that lead to them.
    {
        NfsSession session(connection);
        std::vector<std::string> exports;
        NfsFailure why;
        const bool listed = session.list_exports(&exports, &why);
        bool has_share = false, has_closed = false;
        for (const std::string &e : exports)
        {
            has_share = has_share || e == share;
            has_closed = has_closed || e == closed;
        }
        check(listed && has_share && has_closed, "the exports are listed", why.detail);
        std::vector<NfsEntry> top;
        const std::string first = share.substr(1, share.find('/', 1) - 1);
        check(session.list("/", &top, &why) && !top.empty() && top[0].name == first &&
                  top[0].folder,
              "above the exports, the folders that lead to them", why.detail);
        NfsLocation where;
        check(session.locate(g + "/Game One", &where, &why) && where.exported == share &&
                  where.path == "/PS5/Game One",
              "a folder is found in its export");
        check(!session.locate("/nowhere/PS5", &where, &why) &&
                  why.kind == NfsFailure::Kind::no_export,
              "a folder in no export is \"no_export\"");
    }

    // 2. Reading the NAS directly.
    {
        NasReader nas(connection);
        std::vector<DavEntry> entries;
        NasProblem problem;
        const bool listed = nas.list(g + "/Game One/data/", &entries, &problem);
        bool big = false;
        for (const DavEntry &entry : entries)
            big = big || (entry.name == "big.bin" && !entry.folder && entry.size == (24u << 20) &&
                          entry.path == g + "/Game One/data/big.bin" && entry.modified != "0");
        check(listed && big, "a folder lists with paths, sizes and dates", problem.brief);
        std::string data;
        check(nas.read_file(g + "/A/sce_sys/param.json", 4096, &data) && data == kParam,
              "a small file reads whole");
        check(!nas.read_file(g + "/Game One/data/big.bin", 1000, &data, &problem) &&
                  problem.brief == "too big",
              "a file over the limit is refused", problem.brief);
        std::string why;
        std::string piece;
        check(nas.read_range(g + "/Img.exfat", (5u << 20) + 3, 70000, &piece, &why) &&
                  piece.size() == 70000,
              "a range of a file reads", why);
        check(!nas.list(g + "/Nope/", &entries, &problem) && problem.reached &&
                  problem.brief == "not found",
              "a missing folder is \"not found\"", problem.brief);
    }

    // 3. Test connection.
    {
        const ProbeResult found = probe(connection);
        check(found.state == ProbeResult::State::ok &&
                  found.detail.find("Found 10 games") != std::string::npos &&
                  found.detail.find(nfs_privileged_ports() ? "a privileged port"
                                                           : "an ordinary port") != std::string::npos,
              "Test connection counts the games and names the port", found.detail);
        Connection bare = connection;
        bare.folder = "/";
        const ProbeResult exports = probe(bare);
        check(exports.state == ProbeResult::State::ok &&
                  exports.detail.find("Exports: ") == 0 &&
                  exports.detail.find(share) != std::string::npos,
              "with no folder yet, Test connection names the exports", exports.detail);
    }

    // 4. The library finds what it finds over WebDAV (tests/test_game_count.cpp).
    set_cache_root(work + "/cache");
    {
        const Library library = read_library(connection);
        int folders = 0, images = 0;
        for (const NasGame &game : library.games)
            (game.image ? images : folders) += 1;
        check(library.listed && folders == 6 && images == 4,
              "every layout found", std::to_string(folders) + " folder, " +
                                        std::to_string(images) + " image games; " + library.headline);
        const NasGame *a = find(library.games, "A");
        check(a != nullptr && a->title == "T" && a->title_id == "PPSA00001" && a->stamp.size() > 0,
              "a game's name and title ID come from param.json");
        check(library.covers >= 1, "a cover arrives");
        const NasGame *img = find(library.games, "Img.exfat");
        check(img != nullptr && img->size == (6u << 20) && img->size_known, "an image's size");
        check(find(library.games, "@eaDir") == nullptr && find(library.games, "J") == nullptr &&
                  find(library.games, "F") == nullptr,
              "no games where there are none");
        bool measured = false;
        for (const auto &[entry, bytes] : library.sizes)
            measured = measured || (entry == "Game One" && bytes == one_size);
        check(measured, "a folder game's size is added up, archives left out");
        check(library.finished && library.headline.empty(), "the library finishes cleanly",
              library.headline);

        // Read again: nothing changed, so it all comes from the cache.
        const Library again = read_library(connection);
        check(again.listed && again.games.size() == library.games.size() && again.sizes.empty(),
              "unchanged games come from the cache the second time");
    }

    // 5. Copying in-app (no copy helper).
    const std::string dest = work + "/dest";
    set_transfer_root(work + "/app");
    set_copy_helper("", 0, 0);
    NasGame game_one;
    game_one.entry = "Game One";
    game_one.base = "Game One/sce_sys";
    game_one.title_id = "PPSA01111";
    game_one.size = one_size;
    NasGame image;
    image.entry = "Img.exfat";
    image.image = true;
    image.image_path = "Img.exfat";
    image.title_id = "PPSA03333";
    image.size = 6u << 20;
    const auto game_copied = [&]
    {
        const std::string got = dest + "/Game One";
        bool many = true;
        for (int i = 0; i < 40; ++i)
            many = many && same(one + "/data/many/f" + std::to_string(i) + ".bin",
                                got + "/data/many/f" + std::to_string(i) + ".bin");
        return same(one + "/data/big.bin", got + "/data/big.bin") &&
               same(one + "/sce_sys/param.json", got + "/sce_sys/param.json") &&
               same(one + "/data/sub/tiny.txt", got + "/data/sub/tiny.txt") &&
               there(got + "/data/sub/empty.txt") && there(got + "/data/empty folder") && many &&
               !there(got + "/Game One.zip") && !there(got + "/.DS_Store");
    };
    {
        Transfers transfers;
        transfers.set_space_check([](const std::string &, std::uint64_t *free)
                                  {
                                      *free = 1ull << 40;
                                      return true;
                                  });
        std::string message;
        check(copy(transfers, connection, game_one, dest, &message) && game_copied(),
              "a folder game copies in-app, file for file", message);
        check(copy(transfers, connection, image, dest, &message) &&
                  same(g + "/Img.exfat", dest + "/Img.exfat"),
              "an image copies in-app", message);
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
    }

    // 6. Copying through the copy helper (the host build, through the
    //    stand-in loader), as on the PS5.
    pid_t loader = -1;
    if (!copier.empty())
    {
        file(work + "/loader.json", "{}");
        std::system(("mkdir -p \"" + work + "/ps5root\"").c_str());
        ::setenv("NB_TEST_IDLE", "2", 1); // the helper leaves soon after the tests
        loader = fork();
        if (loader == 0)
        {
            execlp("python3", "python3", "tests/fake_loader.py", std::to_string(loader_port).c_str(),
                   copier.c_str(), std::to_string(copier_port).c_str(), (work + "/ps5root").c_str(),
                   (work + "/loader.json").c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        wait_for_port(loader_port);
        set_copy_helper(copier, loader_port, copier_port);
        Transfers transfers;
        transfers.set_space_check([](const std::string &, std::uint64_t *free)
                                  {
                                      *free = 1ull << 40;
                                      return true;
                                  });
        std::string message;
        check(copy(transfers, connection, game_one, dest, &message) && game_copied(),
              "a folder game copies through the helper, file for file", message);
        const auto mode = [](const std::string &path)
        {
            struct stat st;
            return ::stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0u;
        };
        check(mode(dest + "/Game One/data/big.bin") == 0777 &&
                  mode(dest + "/Game One/data/many/f0.bin") == 0777,
              "the helper's files can be run (0777), big and small",
              std::to_string(mode(dest + "/Game One/data/big.bin")));
        check(copy(transfers, connection, image, dest, &message) &&
                  same(g + "/Img.exfat", dest + "/Img.exfat"),
              "an image copies through the helper", message);
        // (Docker lets anyone bind ports below 1024, so either can come up.)
        check(helper_nfs_port() > 0 && (helper_nfs_port() < 1024) == nfs_privileged_ports(),
              "the helper says which port it connected from",
              std::to_string(helper_nfs_port()));
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
        // Its big file over four connections (two above), chunks written in order.
        set_helper_streams(4);
        check(copy(transfers, connection, game_one, dest, &message) && game_copied(),
              "a big file copies over four connections, file for file", message);
        set_helper_streams(2);
        std::system(("rm -rf \"" + dest + "\"/*").c_str());

        // A file that's gone from the NAS since the library was read.
        NasGame gone = image;
        gone.entry = gone.image_path = "Gone.exfat";
        check(!copy(transfers, connection, gone, dest, &message) &&
                  message.find("isn't on the NAS") != std::string::npos,
              "a missing image is reported", message);
        transfers.remove(0, &message);
    }
    else
    {
        check(false, "the copy helper was given (argv[2])");
    }

    // 7. What goes wrong: an export closed to this PS5, a folder in no
    //    export, and a NAS with NFS off.
    {
        Connection shut = connection;
        shut.folder = closed + "/PS5";
        const Library library = read_library(shut);
        check(!library.listed && library.headline == "The NAS won't let this PS5 in",
              "the library explains an export closed to this PS5", library.headline);
        const ProbeResult shut_probe = probe(shut);
        check(shut_probe.state == ProbeResult::State::failed &&
                  (shut_probe.detail.find("insecure") != std::string::npos) ==
                      !nfs_privileged_ports(),
              "Test connection mentions the insecure option when it matters", shut_probe.detail);
        NasGame a;
        a.entry = "A";
        a.base = "A/sce_sys";
        a.title_id = "PPSA00001";
        a.size = 1;
        Transfers transfers;
        transfers.set_space_check([](const std::string &, std::uint64_t *free)
                                  {
                                      *free = 1ull << 40;
                                      return true;
                                  });
        std::string message;
        check(!copy(transfers, shut, a, dest, &message) &&
                  message.find("refused this PS5") != std::string::npos,
              "a copy explains an export closed to this PS5", message);

        Connection nowhere = connection;
        nowhere.folder = "/nowhere/PS5";
        const Library lost = read_library(nowhere);
        check(!lost.listed && lost.headline == "Not in an NFS export",
              "the library explains a folder in no export", lost.headline);

        Connection off = connection;
        off.port = nothing_port;
        set_nfs_mount_port(nothing_port);
        const ProbeResult off_probe = probe(off);
        check(off_probe.state == ProbeResult::State::failed &&
                  off_probe.headline == "NFS isn't answering",
              "Test connection explains NFS being off", off_probe.headline);
        set_nfs_mount_port(mount_port);
    }

    for (pid_t child : {unfsd, smp, loader})
        if (child > 0)
        {
            ::kill(child, SIGTERM);
            ::waitpid(child, nullptr, 0);
        }
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
