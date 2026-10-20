// NASBridge - The Connection screen refuses changes while a copy needs its
// settings (connection_page.cpp), and allows them again after. Run on a
// computer; drawing isn't exercised.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/connection_page.hpp"

#include <cstdio>
#include <string>

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

bool refused(const ui::Feedback &feedback)
{
    for (const audio::CueEvent &cue : feedback.cues)
        if (cue.cue == audio::Cue::error)
            return true;
    return false;
}

void step(ConnectionPage &page, ui::Feedback &feedback, const InputFrame &input)
{
    page.handle(input, feedback);
    page.update(1.0f / 60.0f);
}

} // namespace

int main()
{
    ui::Fonts fonts;
    ui::Feedback feedback;
    ConnectionPage page(fonts);
    page.enter();
    const int revision = page.revision();

    // Locked: the type list, a field and the folder button all refuse.
    page.set_locked("Locked while NASBridge copies Game.");
    feedback.clear();
    step(page, feedback, press(Action::confirm)); // connection type
    check(refused(feedback), "the connection type can't be opened while locked");
    step(page, feedback, nav(Direction::down)); // server address
    feedback.clear();
    step(page, feedback, press(Action::confirm));
    bool keyboard = false;
    for (const audio::CueEvent &cue : feedback.cues)
        keyboard = keyboard || cue.cue == audio::Cue::modal_open;
    check(refused(feedback) && !keyboard, "a field can't be edited while locked");
    step(page, feedback, nav(Direction::down));  // folder
    step(page, feedback, nav(Direction::right)); // the folder button
    feedback.clear();
    step(page, feedback, press(Action::confirm));
    check(refused(feedback), "the folder can't be browsed while locked");
    check(page.revision() == revision, "nothing was saved while locked");

    // Test connection still runs (it changes nothing).
    step(page, feedback, nav(Direction::left));  // folder
    step(page, feedback, nav(Direction::down));  // user name
    step(page, feedback, nav(Direction::down));  // Test connection
    feedback.clear();
    step(page, feedback, press(Action::confirm));
    // No address is set here, so the test refuses for that reason, not the lock.
    check(page.test_result().headline == "Enter your NAS's address first",
          "Test connection isn't locked", page.test_result().headline);

    // Unlocked: editing works again.
    page.set_locked("");
    step(page, feedback, nav(Direction::up)); // user name
    feedback.clear();
    step(page, feedback, press(Action::confirm));
    keyboard = false;
    for (const audio::CueEvent &cue : feedback.cues)
        keyboard = keyboard || cue.cue == audio::Cue::modal_open || cue.cue == audio::Cue::open;
    check(!refused(feedback) && keyboard, "editing works again once unlocked");

    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
