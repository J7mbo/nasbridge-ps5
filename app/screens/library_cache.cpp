// NASBridge - The saved library: the list, covers and key art, on the PS5.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/library_cache.hpp"

#include "core/save_file.hpp"

#include <cstdio>
#include <cstdlib>

namespace hui::concepts
{

namespace
{

std::string g_root = "/download0/nasbridge"; // set_cache_root() moves it (tests)

std::string cache_dir()
{
    return g_root + "/cache";
}
std::string covers_dir()
{
    return cache_dir() + "/covers";
}
std::string art_dir()
{
    return cache_dir() + "/art";
}
std::string list_path()
{
    return cache_dir() + "/library.dat";
}
constexpr std::uint16_t kVersion = 1;
constexpr const char *kHeader = "nasbridge-library 1";
constexpr std::size_t kFields = 32;

void make_directories()
{
    save::ensure_directory(g_root);
    save::ensure_directory(cache_dir());
    save::ensure_directory(covers_dir());
    save::ensure_directory(art_dir());
}

// The game's name on the NAS, as a file name: 16 hex digits of FNV-1a.
std::string key_of(const std::string &entry)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const char c : entry)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Which NAS and folder a saved list belongs to.
std::string connection_key(const Connection &c)
{
    return std::to_string(static_cast<int>(c.type)) + "|" + c.host + "|" +
           std::to_string(c.effective_port()) + "|" + c.folder + "|" + c.username;
}

std::string escaped(const std::string &text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

std::string unescaped(const std::string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\\' || i + 1 >= text.size())
        {
            out += text[i];
            continue;
        }
        const char c = text[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t tab = line.find('\t', start);
        parts.push_back(unescaped(line.substr(start, tab == std::string::npos ? tab : tab - start)));
        if (tab == std::string::npos)
            return parts;
        start = tab + 1;
    }
}

std::uint64_t number(const std::string &text)
{
    return std::strtoull(text.c_str(), nullptr, 10);
}

void add_file(std::vector<std::string> *fields, const ExfatFile &file)
{
    fields->push_back(file.name);
    fields->push_back(file.directory ? "1" : "0");
    fields->push_back(std::to_string(file.first_cluster));
    fields->push_back(std::to_string(file.length));
    fields->push_back(file.contiguous ? "1" : "0");
}

ExfatFile read_file_fields(const std::vector<std::string> &f, std::size_t at)
{
    ExfatFile file;
    file.name = f[at];
    file.directory = f[at + 1] == "1";
    file.first_cluster = static_cast<std::uint32_t>(number(f[at + 2]));
    file.length = number(f[at + 3]);
    file.contiguous = f[at + 4] == "1";
    return file;
}

} // namespace

std::string data_root()
{
    save::ensure_directory(g_root);
    return g_root;
}

void set_cache_root(const std::string &root)
{
    g_root = root;
}

std::string library_to_text(const Connection &connection, const std::vector<NasGame> &games)
{
    std::string text = std::string(kHeader) + "\n" + escaped(connection_key(connection)) + "\n";
    for (const NasGame &g : games)
    {
        std::vector<std::string> f;
        f.push_back(g.entry);
        f.push_back(g.image ? "1" : "0");
        f.push_back(g.title);
        f.push_back(g.title_id);
        f.push_back(g.version);
        f.push_back(std::to_string(g.size));
        f.push_back(g.size_known ? "1" : "0");
        f.push_back(g.base);
        f.push_back(g.image_path);
        f.push_back(g.inside ? "1" : "0");
        f.push_back(std::to_string(g.geometry.base));
        f.push_back(std::to_string(g.geometry.sector_size));
        f.push_back(std::to_string(g.geometry.cluster_size));
        f.push_back(std::to_string(g.geometry.fat_offset));
        f.push_back(std::to_string(g.geometry.heap_offset));
        f.push_back(std::to_string(g.geometry.cluster_count));
        f.push_back(std::to_string(g.geometry.root_cluster));
        add_file(&f, g.cover_file);
        add_file(&f, g.art_file);
        f.push_back(g.note);
        f.push_back(g.stamp);
        f.push_back(g.folder_stamp);
        f.push_back(g.cover_saved ? "1" : "0");
        f.push_back(g.art_saved ? "1" : "0");
        std::string line;
        for (std::size_t i = 0; i < f.size(); ++i)
            line += (i == 0 ? "" : "\t") + escaped(f[i]);
        text += line + "\n";
    }
    return text;
}

bool library_from_text(const Connection &connection, const std::string &text,
                       std::vector<NasGame> *games)
{
    games->clear();
    std::size_t at = 0;
    const auto next_line = [&](std::string *line) -> bool
    {
        if (at >= text.size())
            return false;
        const std::size_t end = text.find('\n', at);
        *line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        return true;
    };
    std::string line;
    if (!next_line(&line) || line != kHeader)
        return false;
    if (!next_line(&line) || unescaped(line) != connection_key(connection))
        return false; // saved for another NAS or folder
    while (next_line(&line))
    {
        if (line.empty())
            continue;
        const std::vector<std::string> f = split_tabs(line);
        if (f.size() != kFields || f[0].empty())
            return false;
        NasGame g;
        g.entry = f[0];
        g.image = f[1] == "1";
        g.title = f[2];
        g.title_id = f[3];
        g.version = f[4];
        g.size = number(f[5]);
        g.size_known = f[6] == "1";
        g.base = f[7];
        g.image_path = f[8];
        g.inside = f[9] == "1";
        g.geometry.base = number(f[10]);
        g.geometry.sector_size = static_cast<std::uint32_t>(number(f[11]));
        g.geometry.cluster_size = static_cast<std::uint32_t>(number(f[12]));
        g.geometry.fat_offset = number(f[13]);
        g.geometry.heap_offset = number(f[14]);
        g.geometry.cluster_count = static_cast<std::uint32_t>(number(f[15]));
        g.geometry.root_cluster = static_cast<std::uint32_t>(number(f[16]));
        g.cover_file = read_file_fields(f, 17);
        g.art_file = read_file_fields(f, 22);
        g.note = f[27];
        g.stamp = f[28];
        g.folder_stamp = f[29];
        g.cover_saved = f[30] == "1";
        g.art_saved = f[31] == "1";
        games->push_back(std::move(g));
    }
    return true;
}

bool load_library(const Connection &connection, std::vector<NasGame> *games)
{
    games->clear();
    std::string data;
    if (!save::read_file(list_path(), &data, 16u << 20))
        return false;
    const save::Decoded decoded = save::decode(save::Kind::library, data);
    if (!decoded.ok || decoded.version != kVersion)
        return false;
    if (!library_from_text(connection, decoded.payload, games))
    {
        games->clear();
        return false;
    }
    return true;
}

std::string save_library(const Connection &connection, const std::vector<NasGame> &games)
{
    make_directories();
    return save::write_atomic(
        list_path(), save::encode(save::Kind::library, kVersion, library_to_text(connection, games)));
}

std::string cover_path(const std::string &entry)
{
    return covers_dir() + "/" + key_of(entry) + ".png";
}

std::string art_path(const std::string &entry)
{
    return art_dir() + "/" + key_of(entry) + ".png";
}

bool save_picture(const std::string &path, const std::string &data)
{
    make_directories();
    return save::write_atomic(path, data).empty();
}

bool load_picture(const std::string &path, std::string *data, std::size_t max_bytes)
{
    return save::read_file(path, data, max_bytes) && !data->empty();
}

void forget_pictures(const std::string &entry)
{
    std::remove(cover_path(entry).c_str());
    std::remove(art_path(entry).c_str());
}

} // namespace hui::concepts
