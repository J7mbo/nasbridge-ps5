// NASBridge - The NAS connection: saving, loading and checking what was typed.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/connection.hpp"

#include "core/save_file.hpp"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace hui::concepts
{

namespace
{

constexpr const char *kDirectory = "/download0/nasbridge";
constexpr const char *kPath = "/download0/nasbridge/connection.dat";
constexpr std::uint16_t kVersion = 1;

const char *type_key(ConnectionType type)
{
    return type == ConnectionType::ftp ? "ftp" : "webdav";
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i])
            return false;
    return true;
}

// Four numbers from 0 to 255 separated by dots: "192.168.1.20". Names like
// nas.local can't be looked up from the PS5 yet.
bool is_ipv4(const std::string &text)
{
    int parts = 0;
    std::size_t at = 0;
    while (at <= text.size())
    {
        std::size_t end = text.find('.', at);
        if (end == std::string::npos)
            end = text.size();
        const std::size_t length = end - at;
        if (length == 0 || length > 3)
            return false;
        int value = 0;
        for (std::size_t i = at; i < end; ++i)
        {
            if (text[i] < '0' || text[i] > '9')
                return false;
            value = value * 10 + (text[i] - '0');
        }
        if (value > 255 || (length > 1 && text[at] == '0'))
            return false;
        ++parts;
        at = end + 1;
        if (end == text.size())
            break;
    }
    return parts == 4;
}

} // namespace

std::string Connection::display_url() const
{
    std::string url = type == ConnectionType::ftp ? "ftp://" : "http://";
    if (!username.empty())
        url += username + "@";
    url += host.empty() ? "your-nas" : host;
    if (port > 0 && port != default_port())
        url += ":" + std::to_string(port);
    url += folder;
    if (url.back() != '/')
        url += '/';
    return url;
}

bool Connection::load()
{
    *this = Connection{};
    std::string data;
    if (!save::read_file(kPath, &data, 16 * 1024))
        return false;
    const save::Decoded decoded = save::decode(save::Kind::prefs, data);
    if (!decoded.ok)
        return false;
    std::string_view rest = decoded.payload;
    while (!rest.empty())
    {
        const std::size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string_view key = line.substr(0, equals);
        const std::string value(line.substr(equals + 1));
        if (key == "type")
            type = value == "ftp" ? ConnectionType::ftp : ConnectionType::webdav;
        else if (key == "host")
            host = value;
        else if (key == "port")
            port = std::atoi(value.c_str());
        else if (key == "folder")
            folder = value.empty() ? "/" : value;
        else if (key == "username")
            username = value;
        else if (key == "password")
            password = value;
    }
    if (port < 0 || port > 65535)
        port = 0;
    return true;
}

std::string Connection::save() const
{
    // Values come from the on-screen keyboard, which cannot type a newline.
    std::string payload;
    payload += std::string("type=") + type_key(type) + "\n";
    payload += "host=" + host + "\n";
    payload += "port=" + std::to_string(port) + "\n";
    payload += "folder=" + folder + "\n";
    payload += "username=" + username + "\n";
    payload += "password=" + password + "\n";
    save::ensure_directory(kDirectory);
    return save::write_atomic(kPath, save::encode(save::Kind::prefs, kVersion, payload));
}

std::string normalise_host(std::string *text)
{
    std::string host = *text;
    if (starts_with(host, "https://"))
        return "HTTPS isn't supported yet: use the http address";
    if (starts_with(host, "http://"))
        host.erase(0, 7);
    while (!host.empty() && host.back() == '/')
        host.pop_back();
    if (host.find('/') != std::string::npos)
        return "Just the address here: the folder goes below";
    if (host.find(' ') != std::string::npos)
        return "An address can't contain spaces";
    // "nas.local:5005" typed in one go: the port goes to its own field.
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon)
    {
        int port = 0;
        if (!parse_port(host.substr(colon + 1), &port).empty() || port == 0)
            return "Put the port in the Port field";
        host.erase(colon);
    }
    if (!host.empty() && !is_ipv4(host))
        return "Use the NAS's IP address, like 192.168.1.20";
    *text = host;
    return "";
}

std::string normalise_folder(std::string *text)
{
    std::string folder = *text;
    if (folder.empty() || folder.front() != '/')
        folder.insert(folder.begin(), '/');
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    *text = folder;
    return "";
}

std::string parse_port(const std::string &text, int *port)
{
    if (text.empty())
    {
        *port = 0;
        return "";
    }
    long value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return "A port is a number";
        value = value * 10 + (c - '0');
        if (value > 65535)
            return "Use a port from 1 to 65535";
    }
    if (value == 0)
        return "Use a port from 1 to 65535";
    *port = static_cast<int>(value);
    return "";
}

} // namespace hui::concepts
