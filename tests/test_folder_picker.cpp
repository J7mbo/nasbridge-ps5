// NASBridge - Tests for the folder picker (folder_picker.cpp): listing NAS
// folders and the moves Cross, Square, Circle and the D-pad make, run on a
// computer against tests/fake_nas.py. Drawing isn't exercised.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wait_port.hpp"
#include "concepts/folder_picker.hpp"
#include "concepts/http.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace hui;
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

InputFrame press(Action action)
{
    InputFrame input;
    input.pressed = action_bit(action);
    return input;
}

InputFrame nav(Direction direction)
{
    InputFrame input;
    input.nav = direction;
    return input;
}

// Lets the worker answer: frames of nothing until the folder shown has been
// listed for a few frames running (a failed listing may move elsewhere).
void settle(FolderPicker &picker, ui::Feedback &feedback)
{
    int listed = 0;
    for (int i = 0; i < 400 && listed < 4; ++i)
    {
        picker.handle(InputFrame{}, feedback);
        picker.update(1.0f / 60.0f);
        listed = picker.waiting() ? 0 : listed + 1;
        sleep_ms(5);
    }
}

FolderPicker::Result send(FolderPicker &picker, ui::Feedback &feedback, const InputFrame &input)
{
    const FolderPicker::Result result = picker.handle(input, feedback);
    picker.update(1.0f / 60.0f);
    settle(picker, feedback);
    return result;
}

void make(const std::string &path)
{
    std::system(("mkdir -p \"" + path + "\"").c_str());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const std::string work = argv[1];
    const std::string nas = work + "/nas";
    std::system(("rm -rf \"" + work + "\"").c_str());
    // /games: Alpha, beta (sorted without case), Zeta, a file, hidden and
    // NAS-internal folders; Alpha has one folder inside, Zeta none.
    make(nas + "/games/Alpha/Inner");
    make(nas + "/games/beta");
    make(nas + "/games/Zeta");
    make(nas + "/games/.hidden");
    make(nas + "/games/@eaDir");
    make(nas + "/media");
    std::ofstream(nas + "/games/notes.txt") << "a file, not a folder";
    const std::string control = work + "/control.json";
    std::ofstream(control) << "{}";

    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", nas.c_str(), "18081", "10199",
               control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(18081);

    Connection connection;
    connection.host = "127.0.0.1";
    connection.port = 18081;

    // The listing itself.
    {
        const HttpReply reply =
            http_request(connection, "PROPFIND", "/games/",
                         "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n",
                         propfind_body());
        std::vector<std::string> names;
        const bool ok = folders_from_listing(reply.body, "/games/", &names);
        std::string got;
        for (const std::string &name : names)
            got += name + ";";
        check(ok && got == "Alpha;beta;Zeta;",
              "only folders, sorted without case, hidden and NAS-internal left out", got);
    }

    ui::Fonts fonts;
    ui::Feedback feedback;
    FolderPicker picker(fonts);

    // Opens where the box points, goes in and out, chooses.
    picker.open(connection, "/games", feedback);
    settle(picker, feedback);
    check(picker.is_open(), "opens");
    send(picker, feedback, nav(Direction::down)); // beta
    send(picker, feedback, nav(Direction::down)); // Zeta
    send(picker, feedback, nav(Direction::down)); // the end: refused
    send(picker, feedback, nav(Direction::up));   // beta
    send(picker, feedback, nav(Direction::up));   // Alpha
    send(picker, feedback, press(Action::confirm)); // into Alpha
    FolderPicker::Result result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::chosen && picker.chosen() == "/games/Alpha",
          "Cross opens, Square chooses the folder shown", picker.chosen());

    // Up lands on the folder you came from.
    picker.open(connection, "/games/Alpha", feedback);
    settle(picker, feedback);
    send(picker, feedback, press(Action::back));    // up to /games, on Alpha
    send(picker, feedback, nav(Direction::down));   // beta
    send(picker, feedback, nav(Direction::right));  // right opens too
    result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::chosen && picker.chosen() == "/games/beta",
          "Circle goes up onto the folder it left; right opens", picker.chosen());

    // Back to the top, then Circle closes without a choice.
    picker.open(connection, "/games", feedback);
    settle(picker, feedback);
    send(picker, feedback, nav(Direction::left)); // up to /
    result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::chosen && picker.chosen() == "/",
          "the top can be chosen", picker.chosen());
    picker.open(connection, "/", feedback);
    settle(picker, feedback);
    result = send(picker, feedback, press(Action::back));
    check(result == FolderPicker::Result::cancelled && !picker.is_open(),
          "Circle at the top closes without a choice");

    // An empty folder: Cross is refused, Square still chooses it.
    picker.open(connection, "/games/Zeta", feedback);
    settle(picker, feedback);
    feedback.clear();
    send(picker, feedback, press(Action::confirm));
    bool refused = false;
    for (const audio::CueEvent &cue : feedback.cues)
        refused = refused || cue.cue == audio::Cue::error;
    check(refused && picker.is_open(), "nothing to open in an empty folder");
    result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::chosen && picker.chosen() == "/games/Zeta",
          "an empty folder can still be chosen", picker.chosen());

    // A folder that isn't there any more: it starts at the top instead.
    picker.open(connection, "/gone/away", feedback);
    settle(picker, feedback);
    send(picker, feedback, nav(Direction::down)); // games -> media
    send(picker, feedback, nav(Direction::up));   // games
    send(picker, feedback, press(Action::confirm));
    result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::chosen && picker.chosen() == "/games",
          "a missing folder falls back to the top", picker.chosen());

    // A NAS that isn't there: an error, and Square refuses.
    Connection nowhere = connection;
    nowhere.port = 18099;
    picker.open(nowhere, "/", feedback);
    settle(picker, feedback);
    result = send(picker, feedback, press(Action::west));
    check(result == FolderPicker::Result::none && picker.is_open(),
          "an unreadable folder can't be chosen");
    send(picker, feedback, press(Action::back));

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
