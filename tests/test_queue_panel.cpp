// NASBridge - The queue panel (queue_panel.cpp) driven by pad input over a
// real queue (transfer.cpp), against tests/fake_nas.py. Drawing isn't exercised.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/notify.hpp"
#include "concepts/queue_panel.hpp"
#include "concepts/library_cache.hpp"
#include "concepts/smp.hpp"
#include "wait_port.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/stat.h>
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

void write_file(const std::string &path, std::size_t size)
{
    std::system(("mkdir -p \"" + path.substr(0, path.rfind('/')) + "\"").c_str());
    std::ofstream out(path, std::ios::binary);
    std::string block(65536, 'x');
    while (size > 0)
    {
        const std::size_t n = size < block.size() ? size : block.size();
        out.write(block.data(), static_cast<std::streamsize>(n));
        size -= n;
    }
}

bool there(const std::string &path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
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

bool cued(const ui::Feedback &feedback, audio::Cue cue)
{
    for (const audio::CueEvent &event : feedback.cues)
        if (event.cue == cue)
            return true;
    return false;
}

std::string order(const QueueStatus &q)
{
    std::string out;
    for (const QueueEntry &item : q.entries)
        out += item.entry + ";";
    return out;
}

struct Rig
{
    Transfers &transfers;
    QueuePanel &panel;
    ui::Feedback &feedback;
    Connection connection;

    // One frame of input, then a few frames for things to settle.
    void send(const InputFrame &input, int frames = 3)
    {
        feedback.clear();
        panel.handle(input, feedback, connection);
        for (int i = 0; i < frames; ++i)
        {
            panel.update(1.0f / 60.0f, transfers.queue());
            sleep_ms(5);
        }
    }
    void idle(int ms)
    {
        for (int waited = 0; waited < ms; waited += 20)
        {
            panel.update(1.0f / 60.0f, transfers.queue());
            sleep_ms(20);
        }
    }
};

NasGame game(const std::string &entry, const std::string &base, std::uint64_t size)
{
    NasGame g;
    g.entry = entry;
    g.base = base;
    g.size = size;
    return g;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const std::string work = argv[1];
    const std::string nas = work + "/nas";
    const std::string dest = work + "/dest";
    const std::string control = work + "/control.json";
    std::system(("rm -rf \"" + work + "\" && mkdir -p \"" + dest + "\" \"" + work + "/app\"").c_str());
    set_transfer_root(work + "/app");
    set_smp_port(10102);
    set_cache_root(work + "/app");
    set_notify_hook([](const std::string &) {});
    write_file(nas + "/games/A/sce_sys/param.json", 100);
    write_file(nas + "/games/A/big.bin", 12u << 20);
    write_file(nas + "/games/B/sce_sys/param.json", 100);
    write_file(nas + "/games/B/big.bin", 2u << 20);
    write_file(nas + "/games/C/sce_sys/param.json", 100);
    write_file(nas + "/games/C/big.bin", 2u << 20);
    std::ofstream(control) << "{\"throttle\": 2000000}";

    const pid_t server = fork();
    if (server == 0)
    {
        execlp("python3", "python3", "tests/fake_nas.py", nas.c_str(), "18083", "10102",
               control.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    wait_for_port(18083);
    wait_for_port(10102);

    Connection connection;
    connection.host = "127.0.0.1";
    connection.port = 18083;
    connection.folder = "/games/";
    ui::Fonts fonts;
    ui::Feedback feedback;
    Transfers transfers;
    transfers.set_space_check([](const std::string &, std::uint64_t *free)
                              { *free = 1ull << 40; return true; });
    QueuePanel panel(fonts, transfers);
    Rig rig{transfers, panel, feedback, connection};
    std::string why;
    transfers.enqueue(connection, game("A", "A/sce_sys", 12u << 20), "A", dest, &why);
    transfers.enqueue(connection, game("B", "B/sce_sys", 2u << 20), "B", dest, &why);
    transfers.enqueue(connection, game("C", "C/sce_sys", 2u << 20), "C", dest, &why);
    rig.idle(400); // A has started

    panel.open(feedback);
    rig.idle(100);
    check(panel.is_open(), "opens");

    // The game being copied can't be lifted or removed.
    rig.send(press(Action::confirm));
    check(cued(feedback, audio::Cue::error), "the game being copied can't be lifted");
    rig.send(press(Action::north));
    check(cued(feedback, audio::Cue::error) && order(transfers.queue()) == "A;B;C;",
          "the game being copied can't be removed while running");

    // Lift B, carry it below C, set it down.
    rig.send(nav(Direction::down));
    rig.send(press(Action::confirm)); // lift B
    rig.send(nav(Direction::down));   // carry
    check(order(transfers.queue()) == "A;C;B;", "a lifted game is carried down",
          order(transfers.queue()));
    rig.send(nav(Direction::up));     // and back up
    rig.send(nav(Direction::up));     // not above the game being copied
    check(cued(feedback, audio::Cue::error) && order(transfers.queue()) == "A;B;C;",
          "nothing is carried above the game being copied", order(transfers.queue()));
    rig.send(press(Action::confirm)); // set down
    rig.send(nav(Direction::down));   // focus moves, nothing carried
    check(order(transfers.queue()) == "A;B;C;", "set down, moving the focus moves nothing");

    // Square pauses and resumes the queue.
    rig.send(press(Action::west));
    rig.idle(600);
    check(transfers.queue().state == QueueState::paused, "Square pauses");
    rig.send(press(Action::west));
    rig.idle(200);
    check(transfers.queue().state == QueueState::running, "Square resumes");
    rig.send(press(Action::west));
    rig.idle(600);

    // Paused: the front game moves too, and one with files asks before removal.
    rig.send(nav(Direction::up));
    rig.send(nav(Direction::up)); // to A
    rig.send(press(Action::confirm));
    rig.send(nav(Direction::down)); // carry A below B
    rig.send(press(Action::confirm));
    check(order(transfers.queue()) == "B;A;C;", "paused, the front game can be carried",
          order(transfers.queue()));
    check(transfers.queue().entries[1].started, "A has copied files");
    rig.send(press(Action::north)); // remove A: asks
    check(cued(feedback, audio::Cue::modal_open) && order(transfers.queue()) == "B;A;C;",
          "removing a game with copied files asks first");
    rig.send(press(Action::confirm)); // the focus opens on Cancel
    check(order(transfers.queue()) == "B;A;C;", "Cancel keeps it");
    rig.send(press(Action::north));
    rig.send(nav(Direction::right)); // to Remove
    rig.send(press(Action::confirm));
    rig.idle(400);
    check(order(transfers.queue()) == "B;C;" && !there(dest + "/.nasbridge-incoming/A"),
          "Remove takes it out and deletes its copied files", order(transfers.queue()));

    // A game with nothing copied leaves at once.
    rig.send(nav(Direction::down)); // C
    rig.send(press(Action::north));
    check(!cued(feedback, audio::Cue::modal_open) && order(transfers.queue()) == "B;",
          "a game with nothing copied is removed without asking", order(transfers.queue()));

    // Circle closes.
    rig.send(press(Action::back));
    check(!panel.is_open(), "Circle closes");

    // Let B finish, so nothing is left behind.
    std::ofstream(control) << "{}";
    transfers.resume(connection, &why);
    for (int i = 0; i < 300 && transfers.queue().state != QueueState::idle; ++i)
        sleep_ms(20);
    check(transfers.queue().state == QueueState::idle && there(dest + "/B/big.bin"),
          "the rest of the queue finishes");

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
