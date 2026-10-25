// NASBridge - Compressed game images (.ffpfsc): the readers (ffpfsc.cpp,
// pfs.cpp, ufs2.cpp) on images made by MkPFS and Dump Forge, damaged images,
// and the library reading them from tests/fake_nas.py.
// SPDX-License-Identifier: GPL-3.0-or-later
//
//   test_ffpfsc <work folder> <folder of unpacked tests/fixtures images>

#include "wait_port.hpp"
#include "concepts/ffpfsc.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas_library.hpp"
#include "concepts/nas_probe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <signal.h>
#include <sstream>
#include <string>
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

std::string slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void spit(const std::string &path, const std::string &data)
{
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
    std::ofstream(path, std::ios::binary) << data;
}

std::uint32_t crc32(const std::string &data)
{
    std::uint32_t crc = 0xffffffffu;
    for (const char c : data)
    {
        crc ^= static_cast<unsigned char>(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// tests/fixtures/ffpfsc-sce_sys.txt: "name size crc" per line.
struct Expected
{
    std::string name;
    std::size_t size = 0;
    std::uint32_t crc = 0;
};

// Reads from a file held in memory, counting the reads (as range requests).
struct Image
{
    std::string data;
    int reads = 0;
    std::size_t bytes = 0;

    ReadAt reader()
    {
        return [this](std::uint64_t offset, std::size_t length, std::string *out)
        {
            ++reads;
            bytes += length;
            if (offset > data.size() || length > data.size() - offset)
                return false;
            out->assign(data, static_cast<std::size_t>(offset), length);
            return true;
        };
    }
};

// Every fixture opens to the right format, and its sce_sys files come out
// exactly as they went in, for a few hundred kilobytes of reads.
void readers(const std::string &fixtures, const std::vector<Expected> &expected)
{
    const struct
    {
        const char *file;
        const char *format;
    } images[] = {
        {"forge-ffpkg.ffpfsc", "UFS2"},  {"forge-exfat.ffpfsc", "exFAT"},
        {"forge-ffpfs.ffpfsc", "PFS"},   {"mkpfs-exfat.ffpfsc", "exFAT"},
        {"mkpfs-ffpkg.ffpfsc", "UFS2"},  {"mkpfs-pfs.ffpfsc", "PFS"},
        {"mkpfs-nested.ffpfsc", "exFAT"},
    };
    for (const auto &image : images)
    {
        Image file;
        file.data = slurp(fixtures + "/" + image.file);
        FfpfscReader reader(file.reader());
        std::string why;
        const bool opened = reader.open(&why);
        check(opened && reader.inner_format() == image.format,
              std::string(image.file) + " opens (" + image.format + " inside)",
              opened ? reader.inner_format() : why);
        if (!opened)
            continue;
        bool all = reader.sce_sys().size() == expected.size();
        std::string detail;
        for (const Expected &want : expected)
        {
            std::string data;
            if (!reader.read(want.name, 16u << 20, &data, &why))
            {
                all = false;
                detail += want.name + ": " + why + "; ";
                continue;
            }
            if (data.size() != want.size || crc32(data) != want.crc)
            {
                all = false;
                detail += want.name + " differs; ";
            }
        }
        check(all, std::string(image.file) + ": sce_sys read back exactly", detail);
        check(file.reads < 80 && file.bytes < 1024u * 1024u,
              std::string(image.file) + ": a few reads, not the whole image",
              std::to_string(file.reads) + " reads, " + std::to_string(file.bytes) + " bytes");
        std::string data;
        check(!reader.read("missing.png", 1u << 20, &data, &why) &&
                  !reader.read("pic0.png", 1000, &data, &why),
              std::string(image.file) + ": a missing or too-big file is refused");
    }
}

// Damaged images are refused with a reason, never crash (run under the
// sanitizers on a computer when the readers change).
void damaged(const std::string &fixtures)
{
    const std::string good = slurp(fixtures + "/forge-ffpkg.ffpfsc");
    const auto opens = [](const std::string &data, std::string *why)
    {
        Image file;
        file.data = data;
        FfpfscReader reader(file.reader());
        return reader.open(why);
    };
    std::string why;
    check(!opens(std::string(1u << 20, '\0'), &why) && !why.empty(), "zeros aren't an image", why);
    check(!opens(good.substr(0, 300000), &why) && !why.empty(), "a cut-short image is refused", why);
    check(!opens(slurp(fixtures + "/forge-exfat.ffpfsc").substr(0, 65536), &why),
          "an image cut after its header is refused", why);

    // The PFSC container starts at the compressed file's first block; its
    // magic is "PFSC".
    const std::size_t pfsc = good.find("PFSC");
    check(pfsc != std::string::npos && pfsc % 65536 == 0, "the fixture has its PFSC container");
    if (pfsc != std::string::npos)
    {
        std::string bad = good;
        bad[pfsc] = 'X';
        check(!opens(bad, &why) && why.find("PFSC") != std::string::npos,
              "a container without PFSC's magic is refused", why);
        bad = good;
        bad[pfsc + 0x400 + 8] ^= 0x40; // the second block's start
        check(!opens(bad, &why), "a damaged block table is refused", why);
        bad = good;
        for (std::size_t i = 0; i < 64; ++i)
            bad[pfsc + 0x10000 + 2 + i] ^= 0x5a; // inside the first block's zlib stream
        check(!opens(bad, &why), "a block that doesn't inflate is refused", why);
    }

    // Random damage where the structure is: the outer image's header, inodes
    // and folders (its first six 64 KiB blocks start with them) and the
    // container's header and block table.
    const std::size_t places[] = {0, 0x10000, 0x20000, 0x30000, 0x40000, 0x50000, pfsc};
    unsigned seed = 12345;
    const auto next = [&seed]
    {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    };
    int opened = 0;
    for (int round = 0; round < 400; ++round)
    {
        std::string bad = good;
        for (int k = 0; k < 3; ++k)
            bad[places[next() % 7] + next() % 0x480] = static_cast<char>(next());
        Image file;
        file.data = bad;
        FfpfscReader reader(file.reader());
        std::string data;
        if (reader.open(&why))
        {
            ++opened;
            for (const std::string &name : reader.sce_sys())
                reader.read(name, 16u << 20, &data, &why);
        }
    }
    check(true, "400 randomly damaged images read without a crash",
          std::to_string(opened) + " still opened");
}

// The library over a fake NAS: .ffpfsc games on their own and in a folder
// are listed with their names, covers and key art; a broken one is listed
// with why.
void library(const std::string &work, const std::string &fixtures)
{
    const std::string g = work + "/nas/games";
    std::system(("rm -rf \"" + work + "\"").c_str());
    spit(g + "/Quest.ffpfsc", slurp(fixtures + "/forge-ffpkg.ffpfsc"));
    spit(g + "/Folder/PPSA01234.ffpfsc", slurp(fixtures + "/mkpfs-exfat.ffpfsc"));
    spit(g + "/Broken.ffpfsc", std::string(200000, 'x'));
    const std::string control = work + "/control.json";
    spit(control, "{}");

    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", (work + "/nas").c_str(), "18090",
               "10190", control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(18090);

    Connection connection;
    connection.host = "127.0.0.1";
    connection.port = 18090;
    connection.folder = "/games";
    set_cache_root(work + "/cache");

    // The connection test counts them.
    NasProbe probe;
    probe.start(connection, true);
    for (int i = 0; i < 400 && probe.running(); ++i)
        sleep_ms(25);
    check(probe.result().detail == "Found 3 games: all .ffpfsc games.", "Test connection counts them",
          probe.result().detail);

    LibraryLoader loader;
    loader.start(connection, {});
    std::vector<NasGame> games;
    int covers = 0;
    bool finished = false;
    for (int i = 0; i < 800 && !finished; ++i)
    {
        LibraryEvent event;
        while (loader.take(&event))
        {
            if (event.kind == LibraryEvent::Kind::listed)
                games = event.games;
            else if (event.kind == LibraryEvent::Kind::cover && event.image.width > 0)
                ++covers;
            else if (event.kind == LibraryEvent::Kind::finished)
                finished = true;
        }
        sleep_ms(25);
    }
    const auto find = [&games](const std::string &entry) -> const NasGame *
    {
        for (const NasGame &game : games)
            if (game.entry == entry)
                return &game;
        return nullptr;
    };
    const NasGame *quest = find("Quest.ffpfsc");
    const NasGame *folder = find("Folder");
    const NasGame *broken = find("Broken.ffpfsc");
    check(games.size() == 3 && quest != nullptr && folder != nullptr && broken != nullptr,
          "every .ffpfsc is listed", std::to_string(games.size()) + " games");
    for (const NasGame *game : {quest, folder})
    {
        if (game == nullptr)
            continue;
        check(game->image && game->inside && game->title == "Fixture Quest" &&
                  game->title_id == "PPSA01234" && game->version == "01.020.000" &&
                  game->note.empty(),
              game->entry + ": name, title ID and version from inside", game->title + " " + game->note);
        check(game->cover_saved && game->art_saved && game->cover_file.name == "icon0.png" &&
                  game->art_file.name == "pic0.png",
              game->entry + ": cover and key art saved");
    }
    if (folder != nullptr)
        check(folder->image_path == "Folder/PPSA01234.ffpfsc", "an image in a folder is found",
              folder->image_path);
    if (broken != nullptr)
        check(broken->image && !broken->inside &&
                  broken->note.find("compressed image") != std::string::npos,
              "a broken image is listed with why", broken->note);
    check(covers == 2, "both covers reach the screen", std::to_string(covers));

    // The key art, fetched again when its saved copy is gone.
    if (quest != nullptr)
    {
        std::remove(art_path(quest->entry).c_str());
        loader.start_key_art(connection, *quest);
        bool art = false;
        finished = false;
        for (int i = 0; i < 400 && !finished; ++i)
        {
            LibraryEvent event;
            while (loader.take(&event))
            {
                if (event.kind == LibraryEvent::Kind::key_art && event.image.width == 640)
                    art = true;
                else if (event.kind == LibraryEvent::Kind::finished)
                    finished = true;
            }
            sleep_ms(25);
        }
        check(art, "the key art is read from the image again");
    }

    // A second load takes them from the cache, unchanged.
    std::vector<NasGame> saved;
    check(load_library(connection, &saved) && saved.size() == 3, "the cache holds them");
    loader.start(connection, saved);
    std::vector<NasGame> again;
    finished = false;
    for (int i = 0; i < 800 && !finished; ++i)
    {
        LibraryEvent event;
        while (loader.take(&event))
        {
            if (event.kind == LibraryEvent::Kind::listed)
                again = event.games;
            else if (event.kind == LibraryEvent::Kind::finished)
                finished = true;
        }
        sleep_ms(25);
    }
    bool same = again.size() == 3;
    for (const NasGame &game : again)
        same = same && (game.entry == "Broken.ffpfsc" || game.title == "Fixture Quest");
    check(same, "the second load lists them the same");
    loader.stop();

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const std::string work = argv[1];
    const std::string fixtures = argv[2];

    std::vector<Expected> expected;
    std::istringstream lines(slurp(fixtures + "/ffpfsc-sce_sys.txt"));
    std::string name;
    std::size_t size = 0;
    std::string crc;
    while (lines >> name >> size >> crc)
        expected.push_back({name, size, static_cast<std::uint32_t>(std::strtoul(crc.c_str(), nullptr, 16))});
    check(expected.size() == 3, "the fixtures' expected files are there");

    readers(fixtures, expected);
    damaged(fixtures);
    library(work, fixtures);

    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
