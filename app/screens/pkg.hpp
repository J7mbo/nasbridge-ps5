// NASBridge - Looking inside a game package (.pkg) without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A .pkg isn't mounted like a game folder or image: the PS5's own installer
// unpacks it into its storage (see install_helper.hpp). The library still
// shows it like any other game, so this reads its name, title ID, version,
// cover and key art from the package's metadata, a few small reads however
// big the package is.
//
// Layout (multi-byte values big-endian unless said):
//   PS4 package: a "\x7FCNT" header at the start of the file.
//   PS5 package: a "\x7FFIH" header (little-endian) whose 8 bytes at 0x58 are
//     where an embedded "\x7FCNT" header starts; a "\x7FLIH" wrapper (PS5
//     backports) keeps that offset at 0x30.
//   CNT header: entry count at 0x10, the entry table's offset at 0x18 and the
//     content ID ("UP0000-PPSA01234_00-...") at 0x40.
//   Each 32-byte entry: id (0), name offset in the names entry (4), flags (8;
//     0x80000000: encrypted), data offset (16) and size (20), the offsets
//     from the CNT header's start.
//   Entries read here: 0x0200 the names, 0x1000 param.sfo (PS4), 0x2000
//     param.json (PS5), 0x1200 icon0.png, 0x1220 pic0.png.
// The same layout ps5-pkg-manager (itsPLK, GPL-3.0) and LibProsperoPKG read.

#pragma once

#include "concepts/exfat.hpp" // ExfatReader::ReadAt

#include <cstdint>
#include <string>

namespace hui::concepts
{

// A file inside the package: where its bytes are (0 size: not there).
struct PkgPart
{
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

struct PkgInfo
{
    bool ps5 = false;        // a PS5 package (FIH/LIH); otherwise PS4 (CNT)
    std::string content_id;  // "UP0000-PPSA01234_00-XXXXXXXXXXXXXXXX"
    std::string title_id;    // "PPSA01234" / "CUSA01234"
    std::string title;
    std::string version;     // "01.000.000" (PS5) / "01.00" (PS4)
    std::string category;    // param's: "gd" game, "gp" patch, "ac" add-on, ...
    std::string param;       // param.json (PS5), as text; "" for PS4
    PkgPart icon;            // icon0.png
    PkgPart art;             // pic0.png

    // What the package holds, for the library: "update" or "add-on" ("" for
    // a game).
    std::string kind() const;
};

class PkgReader
{
  public:
    using ReadAt = ExfatReader::ReadAt;
    explicit PkgReader(ReadAt read) : read_(std::move(read))
    {
    }

    // Reads the package's header and metadata. False with *why.
    bool open(PkgInfo *info, std::string *why);
    // One of its files (icon, art), at most `max` bytes.
    bool read(const PkgPart &part, std::size_t max, std::string *out, std::string *why);

  private:
    ReadAt read_;
};

// The values a PS4 param.sfo gives (TITLE, TITLE_ID, APP_VER, CATEGORY).
bool read_param_sfo(const std::string &sfo, PkgInfo *info);

} // namespace hui::concepts
