// NASBridge - The connection test's game count (nas_probe.cpp) agrees with
// what the library finds (nas_library.cpp), on one tree of every layout, run
// on a computer against tests/fake_nas.py.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/nas_library.hpp"
#include "concepts/nas_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    check(counted && count.folder_games == 5 && count.image_games == 5 && count.compressed_games == 2 &&
              count.other_folders == 2 && count.unreadable == 0,
          "every layout counted", got);
    check(last_done == 10 && last_total == 10, "progress reaches every folder",
          std::to_string(last_done) + "/" + std::to_string(last_total));
    const std::string line = describe_count(connection, count);
    check(line == "Found 10 games: 5 folder games, 3 .exfat games and 2 .ffpfsc games. 2 folders "
                  "have no game inside.",
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
    for (int i = 0; i < 400 && !listed; ++i)
    {
        LibraryEvent event;
        while (loader.take(&event))
        {
            if (event.kind == LibraryEvent::Kind::listed)
            {
                listed = true;
                for (const NasGame &game : event.games)
                    (game.image ? images : folders) += 1;
            }
        }
        sleep_ms(25);
    }
    loader.stop();
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
