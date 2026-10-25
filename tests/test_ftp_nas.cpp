// NASBridge - The library and the copy queue over FTP, against a real FTP
// server (tests/ftp_server.py, pyftpdlib): listing lines as NAS servers write
// them, what the library finds, and games copied in-app and through the copy
// helper, byte for byte. Twice: a server that lists with MLSD, and one that
// only has LIST and turns away connections past three from one address.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/copy_helper.hpp"
#include "concepts/ftp.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas.hpp"
#include "concepts/nas_library.hpp"
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


void parsing()
{
    FtpEntry e;
    check(parse_mlsd_line("type=file;size=1234;modify=20261025081500;perm=r; big file.bin", &e) &&
              e.name == "big file.bin" && !e.folder && e.size == 1234 && e.modified == "1792916100",
          "an MLSD file line", e.name + " " + e.modified);
    check(parse_mlsd_line("Type=dir;Modify=20261025081500.123;Perm=el; sce_sys\r", &e) && e.folder &&
              e.name == "sce_sys" && e.size == 0,
          "an MLSD folder line, facts in any case");
    check(!parse_mlsd_line("type=cdir;modify=20261025081500; .", &e) &&
              !parse_mlsd_line("type=pdir;modify=20261025081500; ..", &e) &&
              !parse_mlsd_line("type=OS.unix=symlink;size=5; link", &e),
          "MLSD's own folder, its parent and links are left out");
    check(parse_list_line("-rw-r--r--    1 1000     1000     24117248 Oct 25 08:15 big.bin", &e) &&
              e.name == "big.bin" && e.size == 24117248 && !e.folder && e.modified == "Oct 25 08:15",
          "a vsftpd LIST file line", e.name);
    check(parse_list_line("drwxrwxrwx    2 nobody   users        4096 Jan  3  2024 Game  One ", &e) &&
              e.folder && e.name == "Game  One" && e.modified == "Jan 3 2024",
          "a LIST folder line with a year, and spaces in its name", "[" + e.name + "]");
    check(parse_list_line("-rw-r--r-- 1 ftp 12 Mar 9 12:00 no group.txt", &e) && e.size == 12 &&
              e.name == "no group.txt",
          "a LIST line without a group");
    check(parse_list_line("-rw-r--r-- 1 owner group 31 Dec 31 23:59 1 Jan 2025.txt", &e) &&
              e.name == "1 Jan 2025.txt" && e.size == 31,
          "a LIST line whose name looks like a date", e.name);
    check(parse_list_line("10-25-26  08:15AM       <DIR>          PPSA01234", &e) && e.folder &&
              e.name == "PPSA01234",
          "an IIS folder line");
    check(parse_list_line("10-25-26  08:15AM              4567 eboot.bin", &e) && !e.folder &&
              e.size == 4567 && e.name == "eboot.bin",
          "an IIS file line");
    check(!parse_list_line("total 24", &e) &&
              !parse_list_line("lrwxrwxrwx 1 a b 7 Oct 25 08:15 link -> target", &e) &&
              !parse_list_line("drwxr-xr-x 2 a b 4096 Oct 25 08:15 .", &e),
          "totals, links and \".\" are left out");
    check(ftp_path("/Games/PS5/") == "/Games/PS5" && ftp_path("") == "/" && ftp_path("/") == "/",
          "folder settings as FTP paths");
    Connection anonymous;
    anonymous.type = ConnectionType::ftp;
    check(ftp_user(anonymous) == "anonymous" && !ftp_password(anonymous).empty(),
          "no user name signs in as anonymous");
}

// The share's games, every layout, as tests/test_smb_nas.cpp has them.
struct Share
{
    std::string root;   // what the server shares as "/"
    std::string games;  // root + "/Games/PS5"
    std::string one;    // the game copied
    std::uint64_t one_size = 0;
};

Share make_share(const std::string &work)
{
    Share share;
    share.root = work + "/share";
    const std::string g = share.games = share.root + "/Games/PS5";
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
    const std::string one = share.one = g + "/Game One";
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
    share.one_size = (24u << 20) + 903;
    for (int i = 0; i < 40; ++i)
        share.one_size += 1000 + i * 37;
    std::system(("chmod -R a+rX \"" + share.root + "\"").c_str());
    return share;
}

Transfers *new_transfers()
{
    Transfers *transfers = new Transfers;
    transfers->set_space_check([](const std::string &, std::uint64_t *free)
                               {
                                   *free = 1ull << 40;
                                   return true;
                               });
    return transfers;
}

// Everything, against one server. `name` says which in each check.
void against(const std::string &name, const Connection &connection, const Share &share,
             const std::string &work, const std::string &copier, int loader_port, int copier_port)
{
    const std::string tag = " (" + name + ")";
    // 1. Reading the NAS directly.
    {
        NasReader nas(connection);
        std::vector<DavEntry> entries;
        NasProblem problem;
        const bool listed = nas.list("/Games/PS5/Game One/data/", &entries, &problem);
        bool big = false, hidden = false;
        for (const DavEntry &entry : entries)
        {
            big = big || (entry.name == "big.bin" && !entry.folder && entry.size == (24u << 20) &&
                          entry.path == "/Games/PS5/Game One/data/big.bin" && !entry.modified.empty());
            hidden = hidden || entry.name[0] == '.';
        }
        check(listed && big && !hidden, "a folder lists with paths, sizes and dates" + tag, problem.brief);
        // MLSD's dates are numbers (seconds); LIST's are as listed ("Oct 25 08:15").
        const bool numeric = !entries.empty() && entries[0].modified.find_first_not_of("0123456789") ==
                                                     std::string::npos;
        check(numeric == (name == "MLSD"), "listed with " + name + tag,
              entries.empty() ? "" : entries[0].modified);
        std::string data;
        check(nas.read_file("/Games/PS5/A/sce_sys/param.json", 4096, &data) && data == kParam,
              "a small file reads whole" + tag);
        check(!nas.read_file("/Games/PS5/Game One/data/big.bin", 1000, &data, &problem) &&
                  problem.brief == "too big",
              "a file over the limit is refused" + tag, problem.brief);
        std::string why;
        std::string piece, again;
        std::ifstream in(share.games + "/Img.exfat", std::ios::binary);
        std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(nas.read_range("/Games/PS5/Img.exfat", (5u << 20) + 3, 70000, &piece, &why) &&
                  piece == whole.substr((5u << 20) + 3, 70000),
              "a range of a file reads" + tag, why);
        check(nas.read_range("/Games/PS5/Img.exfat", (5u << 20) + 100, 1000, &again, &why) &&
                  again == whole.substr((5u << 20) + 100, 1000),
              "a range just after it comes from what was read ahead" + tag, why);
        check(nas.read_range("/Games/PS5/Img.exfat", (6u << 20) - 10, 10, &again, &why) &&
                  again == whole.substr((6u << 20) - 10, 10),
              "a range at the end of a file" + tag, why);
        check(!nas.read_range("/Games/PS5/Img.exfat", (6u << 20) - 10, 20, &again, &why),
              "a range past the end fails" + tag, why);
        check(!nas.list("/Games/PS5/Nope/", &entries, &problem) && problem.reached &&
                  problem.brief == "not found",
              "a missing folder is \"not found\"" + tag, problem.brief);
        int folders = 0;
        check(nas.walk("/Games/PS5/Game One/",
                       [&](const std::string &, const std::vector<DavEntry> &) { return ++folders, true; },
                       &problem) &&
                  folders == 6,
              "a walk lists every folder of a game" + tag, std::to_string(folders) + " " + problem.brief);
        folders = 0;
        check(!nas.walk("/Games/PS5/",
                        [&](const std::string &, const std::vector<DavEntry> &) { return ++folders < 3; },
                        &problem) &&
                  problem.brief.empty() && folders == 3,
              "a walk stops when asked" + tag, std::to_string(folders) + " " + problem.brief);
        check(nas.list("/Games/PS5/", &entries, &problem) && entries.size() > 5,
              "the reader carries on after a stopped walk" + tag, problem.brief);
    }

    // 2. The library finds what it finds over WebDAV and SMB.
    set_cache_root(work + "/cache-" + name);
    {
        const Library library = read_library(connection);
        int folders = 0, images = 0;
        for (const NasGame &game : library.games)
            (game.image ? images : folders) += 1;
        check(library.listed && folders == 6 && images == 4, "every layout found" + tag,
              std::to_string(folders) + " folder, " + std::to_string(images) + " image games; " +
                  library.headline);
        const NasGame *a = find(library.games, "A");
        check(a != nullptr && a->title == "T" && a->title_id == "PPSA00001" && a->stamp.size() > 0,
              "a game's name and title ID come from param.json" + tag);
        check(library.covers >= 1, "a cover arrives" + tag);
        const NasGame *img = find(library.games, "Img.exfat");
        check(img != nullptr && img->size == (6u << 20) && img->size_known, "an image's size" + tag);
        check(find(library.games, "@eaDir") == nullptr && find(library.games, "J") == nullptr &&
                  find(library.games, "F") == nullptr,
              "no games where there are none" + tag);
        bool measured = false;
        for (const auto &[entry, bytes] : library.sizes)
            measured = measured || (entry == "Game One" && bytes == share.one_size);
        check(measured, "a folder game's size is added up, archives left out" + tag);
        check(library.finished && library.headline.empty(), "the library finishes cleanly" + tag,
              library.headline);
        const Library again = read_library(connection);
        check(again.listed && again.games.size() == library.games.size() && again.sizes.empty(),
              "unchanged games come from the cache the second time" + tag);
    }

    // 3. Copying in-app (no copy helper), then 4. through it.
    const std::string dest = work + "/dest";
    set_transfer_root(work + "/app-" + name);
    NasGame game_one;
    game_one.entry = "Game One";
    game_one.base = "Game One/sce_sys";
    game_one.title_id = "PPSA01111";
    game_one.size = share.one_size;
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
            many = many && same(share.one + "/data/many/f" + std::to_string(i) + ".bin",
                                got + "/data/many/f" + std::to_string(i) + ".bin");
        return same(share.one + "/data/big.bin", got + "/data/big.bin") &&
               same(share.one + "/sce_sys/param.json", got + "/sce_sys/param.json") &&
               same(share.one + "/data/sub/tiny.txt", got + "/data/sub/tiny.txt") &&
               there(got + "/data/sub/empty.txt") && there(got + "/data/empty folder") && many &&
               !there(got + "/Game One.zip") && !there(got + "/.DS_Store");
    };
    {
        set_copy_helper("", 0, 0);
        Transfers *transfers = new_transfers();
        std::string message;
        check(copy(*transfers, connection, game_one, dest, &message) && game_copied(),
              "a folder game copies in-app, file for file" + tag, message);
        check(copy(*transfers, connection, image, dest, &message) &&
                  same(share.games + "/Img.exfat", dest + "/Img.exfat"),
              "an image copies in-app" + tag, message);
        delete transfers;
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
    }
    if (!copier.empty())
    {
        set_copy_helper(copier, loader_port, copier_port);
        Transfers *transfers = new_transfers();
        std::string message;
        check(copy(*transfers, connection, game_one, dest, &message) && game_copied(),
              "a folder game copies through the helper, file for file" + tag, message);
        check(copy(*transfers, connection, image, dest, &message) &&
                  same(share.games + "/Img.exfat", dest + "/Img.exfat"),
              "an image copies through the helper" + tag, message);
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
        // Its big file over four sign-ins (two above), chunks written in order.
        set_helper_streams(4);
        check(copy(*transfers, connection, game_one, dest, &message) && game_copied(),
              "a big file copies over four sign-ins, file for file" + tag, message);
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
        // ... and over one, on a single transfer.
        set_helper_streams(1);
        check(copy(*transfers, connection, game_one, dest, &message) && game_copied(),
              "a big file copies over one sign-in, file for file" + tag, message);
        set_helper_streams(2);
        std::system(("rm -rf \"" + dest + "\"/*").c_str());
        NasGame gone = image;
        gone.entry = gone.image_path = "Gone.exfat";
        check(!copy(*transfers, connection, gone, dest, &message) &&
                  message.find("isn't on the NAS") != std::string::npos,
              "a missing image is reported" + tag, message);
        transfers->remove(0, &message);
        delete transfers;
    }
    else
    {
        check(false, "the copy helper was given (argv[2])");
    }

    // 5. A wrong password: the library and a copy both say so.
    {
        Connection wrong = connection;
        wrong.password = "nope";
        const Library library = read_library(wrong);
        check(!library.listed && library.headline == "Wrong user name or password",
              "the library explains a refused sign-in" + tag, library.headline);
        Transfers *transfers = new_transfers();
        std::string message;
        check(!copy(*transfers, wrong, game_one, dest, &message) &&
                  message.find("sign-in refused") != std::string::npos,
              "a copy explains a refused sign-in" + tag, message);
        delete transfers;
    }
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
    const int mlsd_port = 18160, list_port = 18161, smp_web_port = 18162, smp_port = 10130,
              loader_port = 18164, copier_port = 18165;
    std::system(("rm -rf \"" + work + "\" && mkdir -p \"" + work + "/dest\"").c_str());

    parsing();

    const Share share = make_share(work);
    const auto serve = [&](int port, bool list_only)
    {
        const pid_t pid = fork();
        if (pid == 0)
        {
            const std::string p = std::to_string(port);
            if (list_only)
                execlp("python3", "python3", "tests/ftp_server.py", p.c_str(), share.root.c_str(),
                       "--list-only", "--per-address", "3", static_cast<char *>(nullptr));
            else
                execlp("python3", "python3", "tests/ftp_server.py", p.c_str(), share.root.c_str(),
                       static_cast<char *>(nullptr));
            _exit(127);
        }
        return pid;
    };
    const pid_t mlsd_server = serve(mlsd_port, false);
    const pid_t list_server = serve(list_port, true);
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
    }
    const bool up = wait_for_port(mlsd_port) && wait_for_port(list_port) && wait_for_port(smp_port) &&
                    (copier.empty() || wait_for_port(loader_port));
    check(up, "the FTP servers are up");

    Connection connection;
    connection.type = ConnectionType::ftp;
    connection.host = "127.0.0.1";
    connection.port = mlsd_port;
    connection.folder = "/Games/PS5";
    connection.username = "nbtest";
    connection.password = "secret1";
    against("MLSD", connection, share, work, copier, loader_port, copier_port);
    connection.port = list_port;
    against("LIST", connection, share, work, copier, loader_port, copier_port);

    for (pid_t child : {mlsd_server, list_server, smp, loader})
        if (child > 0)
        {
            ::kill(child, SIGTERM);
            ::waitpid(child, nullptr, 0);
        }
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
