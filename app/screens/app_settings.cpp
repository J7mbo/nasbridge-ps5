// NASBridge - The app's own settings: saving, loading and applying them.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/app_settings.hpp"

#include "concepts/copy_helper.hpp"
#include "concepts/transfer.hpp"

#include "core/save_file.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace hui::concepts
{

namespace
{

constexpr const char *kDirectory = "/download0/nasbridge";
constexpr const char *kPath = "/download0/nasbridge/settings.dat";
constexpr std::uint16_t kVersion = 1;

std::atomic<bool> g_debug{false};
std::atomic<bool> g_sounds{true};
std::atomic<bool> g_keep_awake{true};

int clamp_connections(int value)
{
    return value < 1 ? 1 : value > 4 ? 4 : value;
}

int clamp_small_files(int value)
{
    return value < 1 ? 1 : value > 16 ? 16 : value;
}

} // namespace

bool AppSettings::load()
{
    *this = AppSettings{};
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
        if (key == "connections")
            connections = clamp_connections(std::atoi(value.c_str()));
        else if (key == "small_files")
            small_files = clamp_small_files(std::atoi(value.c_str()));
        else if (key == "debug")
            debug = value == "1";
        else if (key == "sounds")
            sounds = value != "0";
        else if (key == "keep_awake")
            keep_awake = value != "0";
        else if (key == "as_image")
            as_image = value == "1";
    }
    return true;
}

std::string AppSettings::save() const
{
    std::string payload;
    payload += "connections=" + std::to_string(clamp_connections(connections)) + "\n";
    payload += "small_files=" + std::to_string(clamp_small_files(small_files)) + "\n";
    payload += std::string("debug=") + (debug ? "1" : "0") + "\n";
    payload += std::string("sounds=") + (sounds ? "1" : "0") + "\n";
    payload += std::string("keep_awake=") + (keep_awake ? "1" : "0") + "\n";
    payload += std::string("as_image=") + (as_image ? "1" : "0") + "\n";
    save::ensure_directory(kDirectory);
    return save::write_atomic(kPath, save::encode(save::Kind::prefs, kVersion, payload));
}

void AppSettings::apply() const
{
    const int n = clamp_connections(connections);
    set_helper_streams(n); // the copy helper: big files, and small files at once
    set_copy_streams(n);   // copying in-app, when the helper can't run
    set_batch_workers(clamp_small_files(small_files));
    set_copy_as_image(as_image);
    g_debug.store(debug);
    g_sounds.store(sounds);
    g_keep_awake.store(keep_awake);
}

bool debug_info()
{
    return g_debug.load();
}

bool sounds_on()
{
    return g_sounds.load();
}

bool keep_awake_on()
{
    return g_keep_awake.load();
}

} // namespace hui::concepts
