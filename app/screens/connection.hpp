// NASBridge - The NAS connection: what the Connection screen edits.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saved in the app's own save area on the PS5 (/download0, which only this
// app can reach), as key=value lines in the kit's checked save container.
// The password is stored as typed: it never leaves the console except to
// sign in to the NAS.

#pragma once

#include <string>

namespace hui::concepts
{

enum class ConnectionType : int
{
    webdav = 0,
    ftp = 1, // listed as "coming later"; not selectable yet
};

struct Connection
{
    ConnectionType type = ConnectionType::webdav;
    std::string host;          // "192.168.1.20" or "nas.local"; empty: not set up
    int port = 0;              // 0: the type's default
    std::string folder = "/";  // where the games are, always starting with "/"
    std::string username;      // empty: no sign-in
    std::string password;

    bool configured() const
    {
        return !host.empty();
    }
    int default_port() const
    {
        return type == ConnectionType::ftp ? 21 : 80;
    }
    int effective_port() const
    {
        return port > 0 ? port : default_port();
    }
    // "http://nas.local:5005/games/" (the user name, not the password, when set).
    std::string display_url() const;

    // Loads the saved connection; false (and defaults) when there is none.
    bool load();
    // Writes it; returns an error, or "" when it was saved.
    std::string save() const;
};

// Tidies what was typed. Each returns "" when the text is acceptable (and
// rewrites *text to the form that is kept), otherwise a message for the
// field's error line.
std::string normalise_host(std::string *text);
std::string normalise_folder(std::string *text);
std::string parse_port(const std::string &text, int *port);

} // namespace hui::concepts
