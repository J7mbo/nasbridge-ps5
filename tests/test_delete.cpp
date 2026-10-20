// NASBridge - The delete helper (tools/helper/helper.c) driven by the client
// (delete_helper.cpp) through a stand-in ELF loader (tests/fake_loader.py).
// The client sends the host build of the helper to the fake loader, which runs
// it, so the real wire protocol and the real deletion logic are both tested.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/delete_helper.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace hui::concepts;

namespace
{

int g_failures = 0;

void check(bool ok, const std::string &what, const std::string &detail = "")
{
    std::printf("%s %s%s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  [",
                detail.c_str(), detail.empty() ? "" : "]");
    if (!ok)
        ++g_failures;
}

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

bool there(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

void write_file(const std::string &path, std::size_t size)
{
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
    std::ofstream out(path, std::ios::binary);
    std::string block(4096, 'x');
    while (size > 0)
    {
        const std::size_t n = size < block.size() ? size : block.size();
        out.write(block.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
}

// A game folder on a drive under `root`, with a param.json naming `title_id`.
void make_game(const std::string &root, const std::string &rel, const std::string &title_id,
               std::size_t big)
{
    write_file(root + rel + "/sce_sys/param.json", 0);
    std::ofstream(root + rel + "/sce_sys/param.json")
        << "{\"titleId\": \"" << title_id << "\"}";
    write_file(root + rel + "/game-data.bin", big);
    write_file(root + rel + "/extra/more.bin", 2048);
}

std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Progress
{
    std::uint64_t last_done = 0;
    std::uint64_t total = 0;
    int calls = 0;
    void operator()(std::uint64_t done, std::uint64_t t)
    {
        last_done = done;
        total = t;
        ++calls;
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <work-dir> <host-helper-binary>\n", argv[0]);
        return 2;
    }
    const std::string work = argv[1];
    const std::string helper = argv[2];
    const std::string root = work + "/root"; // the helper's filesystem root
    const std::string control = work + "/control.json";
    std::system(("rm -rf \"" + work + "\" && mkdir -p \"" + root + "\"").c_str());
    std::ofstream(control) << "{}";

    const auto set_control = [&](const std::string &json)
    { std::ofstream(control, std::ios::trunc) << json; };

    const int loader_port = 9230;
    const int helper_port = 9231;
    const pid_t loader = fork();
    if (loader == 0)
    {
        execlp("python3", "python3", "tests/fake_loader.py", std::to_string(loader_port).c_str(),
               helper.c_str(), std::to_string(helper_port).c_str(), root.c_str(), control.c_str(),
               static_cast<char *>(nullptr));
        _exit(127);
    }
    sleep_ms(1200);

    // The client reads this file and sends it to the loader; the loader ignores
    // the bytes and runs the host helper, so the host helper binary stands in.
    set_delete_helper(helper, loader_port, helper_port);

    // 1. A game folder: files gone, game uninstalled, progress seen.
    {
        make_game(root, "/mnt/ext0/games/Alpha", "PPSA01111", 200000);
        Progress p;
        const DeleteResult r =
            delete_with_helper("PPSA01111", "/mnt/ext0/games/Alpha", std::ref(p));
        check(r.ok, "a game folder is deleted", r.stage + " " + r.why);
        check(!there(root + "/mnt/ext0/games/Alpha"), "its files are gone");
        check(read_file(root + "/uninstalled.txt").find("PPSA01111") != std::string::npos,
              "it's uninstalled from the home screen");
        check(p.total > 0 && p.last_done == p.total, "progress ran to the total",
              std::to_string(p.last_done) + "/" + std::to_string(p.total));
    }

    // 2. A game image (one file).
    {
        write_file(root + "/mnt/ext0/images/Big.exfat", 300000);
        Progress p;
        const DeleteResult r =
            delete_with_helper("PPSA02222", "/mnt/ext0/images/Big.exfat", std::ref(p));
        check(r.ok && !there(root + "/mnt/ext0/images/Big.exfat"), "a game image is deleted",
              r.stage + " " + r.why);
        check(read_file(root + "/uninstalled.txt").find("PPSA02222") != std::string::npos,
              "the image's game is uninstalled");
    }

    // 3. An installed PKG: no files to delete, just uninstalled.
    {
        Progress p;
        const DeleteResult r = delete_with_helper("PPSA03333", "", std::ref(p));
        check(r.ok, "a PKG with no source files is just uninstalled", r.stage + " " + r.why);
        check(read_file(root + "/uninstalled.txt").find("PPSA03333") != std::string::npos,
              "the PKG is uninstalled");
    }

    // 4. The game is open: refused before anything is touched.
    {
        make_game(root, "/mnt/ext0/games/Running", "PPSA04444", 50000);
        set_control("{\"running\": \"PPSA04444\"}");
        const DeleteResult r = delete_with_helper("PPSA04444", "/mnt/ext0/games/Running",
                                                  std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r.ok && r.stage == "request", "an open game is refused", r.stage + " " + r.why);
        check(there(root + "/mnt/ext0/games/Running"), "its files are left alone");
        set_control("{}");
    }

    // 5. A path outside storage, and a folder that isn't a game: refused,
    //    nothing deleted.
    {
        const DeleteResult r = delete_with_helper("PPSA05555", "/etc/passwd",
                                                  std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r.ok && r.stage == "request", "a path outside game storage is refused",
              r.stage + " " + r.why);

        write_file(root + "/mnt/ext0/games/NotAGame/some.bin", 1000);
        const DeleteResult r2 = delete_with_helper(
            "PPSA05555", "/mnt/ext0/games/NotAGame",
            std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r2.ok && r2.stage == "request", "a folder without a matching game is refused",
              r2.stage + " " + r2.why);
        check(there(root + "/mnt/ext0/games/NotAGame"), "that folder is left alone");
    }

    // 6. A storage root itself (too shallow) is refused.
    {
        const DeleteResult r = delete_with_helper("PPSA06666", "/mnt/ext0/games",
                                                  std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r.ok && r.stage == "request", "a storage folder, not a game, is refused",
              r.stage + " " + r.why);
    }

    // 7. The system uninstall refuses: files are deleted, but the helper says
    //    the home-screen entry is still there.
    {
        make_game(root, "/mnt/ext0/games/Stuck", "PPSA07777", 40000);
        set_control("{\"uninstall_fails\": true}");
        const DeleteResult r = delete_with_helper("PPSA07777", "/mnt/ext0/games/Stuck",
                                                  std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r.ok && r.stage == "uninstall", "a refused uninstall is reported as such",
              r.stage + " " + r.why);
        check(!there(root + "/mnt/ext0/games/Stuck"), "its files were still deleted");
        set_control("{}");
    }

    // 8. The loader isn't answering: reported as a loader problem.
    {
        kill(loader, SIGTERM);
        waitpid(loader, nullptr, 0);
        const DeleteResult r = delete_with_helper("PPSA08888", "/mnt/ext0/games/Gone",
                                                  std::function<void(std::uint64_t, std::uint64_t)>());
        check(!r.ok && r.stage == "loader", "no loader is reported as a loader problem",
              r.stage + " " + r.why);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
