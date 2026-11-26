// NASBridge - Looking inside a game package (.pkg) without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/pkg.hpp"

#include "concepts/nas_library.hpp" // read_param_json

#include <cstring>

namespace hui::concepts
{

namespace
{

constexpr std::uint32_t kEncrypted = 0x80000000u;
constexpr std::uint32_t kMaxEntries = 4096;
constexpr std::uint32_t kMaxNames = 256 * 1024;
constexpr std::uint32_t kMaxParam = 256 * 1024;

std::uint32_t be32(const std::string &bytes, std::size_t at)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data() + at);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}

std::uint32_t le32(const std::string &bytes, std::size_t at)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data() + at);
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

std::uint16_t le16(const std::string &bytes, std::size_t at)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data() + at);
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint64_t le64(const std::string &bytes, std::size_t at)
{
    return std::uint64_t(le32(bytes, at)) | (std::uint64_t(le32(bytes, at + 4)) << 32);
}

// A printable string from a fixed field (stops at the first byte that isn't).
std::string printable(const std::string &bytes, std::size_t at, std::size_t most)
{
    std::string out;
    for (std::size_t i = at; i < bytes.size() && i < at + most; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 32 || c > 126)
            break;
        out += static_cast<char>(c);
    }
    return out;
}

} // namespace

std::string PkgInfo::kind() const
{
    if (category.compare(0, 2, "gp") == 0)
        return "update";
    if (category.compare(0, 2, "ac") == 0 || category.compare(0, 2, "al") == 0)
        return "add-on";
    return "";
}

bool read_param_sfo(const std::string &sfo, PkgInfo *info)
{
    if (sfo.size() < 0x14 || sfo.compare(0, 4, std::string("\0PSF", 4)) != 0)
        return false;
    const std::uint32_t keys = le32(sfo, 0x08);
    const std::uint32_t data = le32(sfo, 0x0C);
    const std::uint32_t count = le32(sfo, 0x10);
    if (keys > sfo.size() || data > sfo.size() || count > 1024 ||
        0x14 + std::uint64_t(count) * 16 > sfo.size())
        return false;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::size_t at = 0x14 + std::size_t(i) * 16;
        const std::size_t key = keys + le16(sfo, at);
        const std::uint16_t format = le16(sfo, at + 2);
        const std::uint32_t length = le32(sfo, at + 4);
        const std::size_t value = std::size_t(data) + le32(sfo, at + 12);
        if (key >= sfo.size() || value > sfo.size() || length > sfo.size() - value)
            continue;
        if (format != 0x0204 && format != 0x0004) // UTF-8 text
            continue;
        const std::string name = printable(sfo, key, 64);
        std::string text = sfo.substr(value, length);
        while (!text.empty() && text.back() == '\0')
            text.pop_back();
        if (name == "TITLE")
            info->title = text;
        else if (name == "TITLE_ID")
            info->title_id = text;
        else if (name == "APP_VER")
            info->version = text;
        else if (name == "VERSION" && info->version.empty())
            info->version = text;
        else if (name == "CATEGORY")
            info->category = text;
    }
    return true;
}

bool PkgReader::open(PkgInfo *info, std::string *why)
{
    *info = PkgInfo();
    std::string head;
    if (!read_(0, 0x100, &head) || head.size() < 0x80)
    {
        *why = "the package couldn't be read";
        return false;
    }
    std::uint64_t cnt = 0;
    if (head.compare(0, 4, "\x7F" "CNT") == 0)
    {
        cnt = 0;
    }
    else if (head.compare(0, 4, "\x7F" "FIH") == 0 || head.compare(0, 4, "\x7F" "LIH") == 0)
    {
        info->ps5 = true;
        cnt = le64(head, head[1] == 'F' ? 0x58 : 0x30);
        if (cnt < 0x80)
        {
            *why = "the package's header doesn't say where its contents are";
            return false;
        }
    }
    else
    {
        *why = "not a PS4 or PS5 package";
        return false;
    }

    std::string header;
    if (!read_(cnt, 0x80, &header) || header.size() < 0x80 ||
        header.compare(0, 4, "\x7F" "CNT") != 0)
    {
        *why = "the package's contents header couldn't be read";
        return false;
    }
    info->content_id = printable(header, 0x40, 0x30);
    const std::uint32_t count = be32(header, 0x10);
    const std::uint32_t table = be32(header, 0x18);
    if (count == 0 || count > kMaxEntries)
    {
        *why = "the package's file table looks damaged";
        return false;
    }
    std::string entries;
    if (!read_(cnt + table, std::size_t(count) * 32, &entries) || entries.size() < std::size_t(count) * 32)
    {
        *why = "the package's file table couldn't be read";
        return false;
    }

    // The names, for packages whose ids differ from the usual ones.
    std::string names;
    for (std::uint32_t i = 0; i < count; ++i)
        if (be32(entries, i * 32) == 0x0200)
        {
            const std::uint32_t size = be32(entries, i * 32 + 20);
            if (size > 0 && size <= kMaxNames)
                read_(cnt + be32(entries, i * 32 + 16), size, &names);
            break;
        }

    std::string param_sfo;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::size_t at = std::size_t(i) * 32;
        const std::uint32_t id = be32(entries, at);
        const std::uint32_t name_at = be32(entries, at + 4);
        const std::uint32_t flags = be32(entries, at + 8);
        const PkgPart part{cnt + be32(entries, at + 16), be32(entries, at + 20)};
        const std::string name = name_at < names.size() ? printable(names, name_at, 64) : "";
        if ((flags & kEncrypted) != 0 || part.size == 0)
            continue;
        if ((id == 0x2000 || name == "param.json") && info->param.empty() && part.size <= kMaxParam)
            read_(part.offset, part.size, &info->param);
        else if ((id == 0x1000 || name == "param.sfo") && param_sfo.empty() && part.size <= kMaxParam)
            read_(part.offset, part.size, &param_sfo);
        else if ((id == 0x1200 || name == "icon0.png") && info->icon.size == 0)
            info->icon = part;
        else if ((id == 0x1220 || name == "pic0.png") && info->art.size == 0)
            info->art = part;
    }

    if (!info->param.empty())
    {
        NasGame game;
        read_param_json(info->param, &game);
        info->title = game.title;
        info->title_id = game.title_id;
        info->version = game.version;
        // "category": "gd" a game, "gp" a patch, "ac" an add-on.
        const std::size_t key = info->param.find("\"category\"");
        if (key != std::string::npos)
        {
            const std::size_t open = info->param.find('"', info->param.find(':', key) + 1);
            const std::size_t close = open == std::string::npos ? open : info->param.find('"', open + 1);
            if (close != std::string::npos && close - open <= 8)
                info->category = info->param.substr(open + 1, close - open - 1);
        }
    }
    else if (!param_sfo.empty())
    {
        read_param_sfo(param_sfo, info);
    }
    // A patch: the CNT header's content flags (0x78) carry it on both.
    const std::uint32_t content_flags = be32(header, 0x78);
    if (info->category.empty() && (content_flags & 0x40100000u) != 0)
        info->category = "gp";
    if (info->title_id.empty() && info->content_id.size() >= 16)
        info->title_id = info->content_id.substr(7, 9); // "UP0000-PPSA01234_00-..."
    return true;
}

bool PkgReader::read(const PkgPart &part, std::size_t max, std::string *out, std::string *why)
{
    if (part.size == 0)
    {
        *why = "not in the package";
        return false;
    }
    if (part.size > max)
    {
        *why = "too big";
        return false;
    }
    if (!read_(part.offset, part.size, out) || out->size() != part.size)
    {
        *why = "couldn't be read from the package";
        return false;
    }
    return true;
}

} // namespace hui::concepts
