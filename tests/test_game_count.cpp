// NASBridge - The connection test's game count (nas_probe.cpp) agrees with
// what the library finds (nas_library.cpp), on one tree of every layout, run
// on a computer against tests/fake_nas.py.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas_library.hpp"
#include "concepts/nas_probe.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
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
    std::ofstream(path) << text;
}

const char *kParam = R"({"titleId":"PPSA00001","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"T"}}})";

// A 1x1 PNG, for a package's cover.
const unsigned char kPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};

void put_be32(std::string &b, std::size_t at, std::uint32_t v)
{
    b[at] = static_cast<char>(v >> 24);
    b[at + 1] = static_cast<char>(v >> 16);
    b[at + 2] = static_cast<char>(v >> 8);
    b[at + 3] = static_cast<char>(v);
}

// A package as pkg.hpp describes it: PS5 (an FIH header, the CNT header at
// 0x10000, param.json) or PS4 (CNT at the start, param.sfo), with a cover,
// `padding` bytes of game data after.
std::string make_pkg(bool ps5, const std::string &content_id, const std::string &param,
                     std::size_t padding)
{
    const std::size_t cnt = ps5 ? 0x10000 : 0;
    const std::string names = std::string("\0", 1) + (ps5 ? "param.json" : "param.sfo") +
                              std::string("\0", 1) + "icon0.png" + std::string("\0", 1);
    const std::string icon(reinterpret_cast<const char *>(kPng), sizeof(kPng));
    struct Entry
    {
        std::uint32_t id, name;
        const std::string *data;
    };
    const Entry entries[] = {{0x0200, 0, &names}, {ps5 ? 0x2000u : 0x1000u, 1, &param},
                             {0x1200, ps5 ? 12u : 11u, &icon}};
    std::string b(cnt + 0x200, '\0');
    if (ps5)
    {
        b.replace(0, 4, "\x7F" "FIH");
        b[0x58 + 2] = 0x01; // 0x10000, little-endian
    }
    b.replace(cnt, 4, "\x7F" "CNT");
    put_be32(b, cnt + 0x10, 3);
    put_be32(b, cnt + 0x18, 0x100);
    b.replace(cnt + 0x40, content_id.size(), content_id);
    for (int i = 0; i < 3; ++i)
    {
        const std::size_t at = cnt + 0x100 + std::size_t(i) * 32;
        put_be32(b, at, entries[i].id);
        put_be32(b, at + 4, entries[i].name);
        put_be32(b, at + 16, static_cast<std::uint32_t>(b.size() - cnt));
        put_be32(b, at + 20, static_cast<std::uint32_t>(entries[i].data->size()));
        b += *entries[i].data;
    }
    return b + std::string(padding, 'g');
}

// A PS4 param.sfo with these text values.
std::string make_sfo(const std::vector<std::pair<std::string, std::string>> &values)
{
    std::string keys, data, table;
    for (const auto &[key, value] : values)
    {
        std::string entry(16, '\0');
        const auto le16 = [&](std::size_t at, std::uint32_t v)
        {
            entry[at] = static_cast<char>(v);
            entry[at + 1] = static_cast<char>(v >> 8);
        };
        const auto le32 = [&](std::size_t at, std::uint32_t v)
        {
            for (int k = 0; k < 4; ++k)
                entry[at + k] = static_cast<char>(v >> (8 * k));
        };
        le16(0, static_cast<std::uint32_t>(keys.size()));
        le16(2, 0x0204);
        le32(4, static_cast<std::uint32_t>(value.size() + 1));
        le32(8, static_cast<std::uint32_t>(value.size() + 1));
        le32(12, static_cast<std::uint32_t>(data.size()));
        table += entry;
        keys += key + std::string("\0", 1);
        data += value + std::string("\0", 1);
    }
    std::string head(0x14, '\0');
    head.replace(0, 4, std::string("\0PSF", 4));
    const auto at = [&](std::size_t pos, std::uint32_t v)
    {
        for (int k = 0; k < 4; ++k)
            head[pos + k] = static_cast<char>(v >> (8 * k));
    };
    at(0x08, static_cast<std::uint32_t>(0x14 + table.size()));
    at(0x0C, static_cast<std::uint32_t>(0x14 + table.size() + keys.size()));
    at(0x10, static_cast<std::uint32_t>(values.size()));
    return head + table + keys + data;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const std::string work = argv[1];
    const std::string g = work + "/nas/games";
    std::system(("rm -rf \"" + work + "\"").c_str());
    file(g + "/A/sce_sys/param.json", kParam);           // folder game
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
    file(g + "/K.ffpfsc");                               // a compressed image
    file(g + "/L/L.ffpfsc");                             // ... in a folder
    file(g + "/notes.txt");                              // a file: not a game
    // Packages (.pkg): PS5 and PS4 ones in the games folder, a game with its
    // update in a folder (the biggest is the game), and a folder whose image
    // wins over its package.
    const char *kPkgParam =
        R"({"titleId":"PPSA09999","contentVersion":"01.000.000","category":"gd",)"
        R"("localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Pkg Game"}}})";
    file(g + "/M.pkg", make_pkg(true, "UP0000-PPSA09999_00-PKGGAME000000000", kPkgParam, 1000));
    file(g + "/O.pkg",
         make_pkg(false, "UP0000-CUSA01234_00-OLDGAME000000000",
                  make_sfo({{"APP_VER", "01.00"}, {"CATEGORY", "gd"}, {"TITLE", "Old Game"},
                            {"TITLE_ID", "CUSA01234"}}),
                  100));
    file(g + "/N/patch.pkg", make_pkg(true, "UP0000-PPSA07777_00-PATCH00000000000",
                                      R"({"titleId":"PPSA07777","category":"gp"})", 10));
    file(g + "/N/base.pkg", make_pkg(true, "UP0000-PPSA07777_00-BASE000000000000",
                                     R"({"titleId":"PPSA07777","category":"gd","localizedParameters":)"
                                     R"({"defaultLanguage":"en-US","en-US":{"titleName":"Base"}}})",
                                     5000));
    file(g + "/P/P.exfat");
    file(g + "/P/P-update.pkg", make_pkg(true, "UP0000-PPSA06666_00-X000000000000000", "{}", 0));
    const std::string control = work + "/control.json";
    file(control, "{}");

    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", (work + "/nas").c_str(), "18082",
               "10198", control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(18082);

    Connection connection;
    connection.host = "127.0.0.1";
    connection.port = 18082;
    connection.folder = "/games";

    // The test's count, with its progress.
    const HttpReply reply =
        http_request(connection, "PROPFIND", "/games/",
                     "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
                     propfind_body());
    std::vector<DavEntry> top;
    parse_listing(reply.body, "/games/", &top);
    GameCount count;
    int last_done = -1;
    int last_total = -1;
    const bool counted = count_games(
        connection, top, nullptr,
        [&](int done, int total)
        {
            last_done = done;
            last_total = total;
        },
        &count);
    char got[96];
    std::snprintf(got, sizeof(got), "folder %d, image %d, other %d, unreadable %d",
                  count.folder_games, count.image_games, count.other_folders, count.unreadable);
    check(counted && count.folder_games == 5 && count.image_games == 9 && count.compressed_games == 2 &&
              count.package_games == 3 && count.other_folders == 2 && count.unreadable == 0,
          "every layout counted", got);
    check(last_done == 12 && last_total == 12, "progress reaches every folder",
          std::to_string(last_done) + "/" + std::to_string(last_total));
    const std::string line = describe_count(connection, count);
    check(line == "Found 14 games: 5 folder games, 4 .exfat games, 2 .ffpfsc games and 3 .pkg "
                  "games. 2 folders have no game inside.",
          "the test's line", line);
    GameCount stopped;
    check(!count_games(connection, top, [] { return true; }, nullptr, &stopped),
          "a count can be stopped");

    // The library, on the same tree.
    set_cache_root(work + "/cache");
    LibraryLoader loader;
    loader.start(connection, {});
    int folders = 0;
    int images = 0;
    bool listed = false;
    bool ready = false;
    std::vector<NasGame> found;
    std::vector<std::string> covers;
    for (int i = 0; i < 400 && !ready; ++i)
    {
        LibraryEvent event;
        while (loader.take(&event))
        {
            if (event.kind == LibraryEvent::Kind::listed)
            {
                listed = true;
                found = event.games;
                for (const NasGame &game : event.games)
                    (game.image ? images : folders) += 1;
            }
            if (event.kind == LibraryEvent::Kind::cover)
                covers.push_back(event.entry);
            if (event.kind == LibraryEvent::Kind::ready || event.kind == LibraryEvent::Kind::finished)
                ready = true;
        }
        sleep_ms(25);
    }
    loader.stop();
    const auto game_called = [&](const std::string &entry) -> const NasGame *
    {
        for (const NasGame &game : found)
            if (game.entry == entry)
                return &game;
        return nullptr;
    };
    const auto has_cover = [&](const std::string &entry)
    { return std::find(covers.begin(), covers.end(), entry) != covers.end(); };
    const NasGame *m = game_called("M.pkg");
    check(m != nullptr && m->title == "Pkg Game" && m->title_id == "PPSA09999" &&
              m->version == "01.000.000" && m->image_path == "M.pkg" && has_cover("M.pkg"),
          "a PS5 package's name, title ID, version and cover",
          m ? m->title + " " + m->title_id + " " + m->version + " " + m->note : "missing");
    const NasGame *o = game_called("O.pkg");
    check(o != nullptr && o->title == "Old Game" && o->title_id == "CUSA01234" &&
              o->version == "01.00" && has_cover("O.pkg"),
          "a PS4 package's, from its param.sfo",
          o ? o->title + " " + o->title_id + " " + o->version + " " + o->note : "missing");
    const NasGame *n = game_called("N");
    check(n != nullptr && n->image_path == "N/base.pkg" && n->title == "Base",
          "of a game and its update in a folder, the game is listed",
          n ? n->image_path + " " + n->title : "missing");
    const NasGame *pfolder = game_called("P");
    check(pfolder != nullptr && pfolder->image_path == "P/P.exfat",
          "an image wins over a package beside it", pfolder ? pfolder->image_path : "missing");
    check(listed && folders == count.folder_games && images == count.image_games,
          "the library finds the same games",
          std::to_string(folders) + " folder, " + std::to_string(images) + " image");

    // The whole test through NasProbe: quick (at launch) and counting.
    NasProbe probe;
    probe.start(connection);
    for (int i = 0; i < 200 && probe.running(); ++i)
        sleep_ms(25);
    check(probe.result().detail == "Reached /games. Test connection counts the games in it.",
          "the quick test doesn't count folders as games", probe.result().detail);
    probe.start(connection, true);
    for (int i = 0; i < 400 && probe.running(); ++i)
        sleep_ms(25);
    check(probe.result().detail == line, "Test connection shows the count", probe.result().detail);

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
