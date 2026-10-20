// NASBridge - Reads the games on the NAS, in the background, through a cache.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The library opens from what was saved last time (library_cache.cpp): the
// list at once, then the covers from the PS5's own storage. Meanwhile the NAS
// is checked: the games folder is listed, and each game is compared with its
// saved copy by the dates and sizes the NAS reports. Only new or changed
// games are read again (param.json for the name, title ID and version, the
// cover, and the key art, all saved for next time). Folder sizes come last.
//
// Game folders have sce_sys in them (up to four folders down). .exfat images,
// on their own or alone in a folder, are looked inside with HTTP Range
// requests (exfat.cpp) without copying them. Folders with no game inside
// (backports, patches, empty folders) are left out.
//
// Results arrive as events the library takes on the UI thread, which is the
// only thread that touches OpenGL. A load can be abandoned at any time (the
// settings changed): its thread finishes on its own and frees what it used.

#pragma once

#include "concepts/connection.hpp"
#include "concepts/exfat.hpp"
#include "concepts/png.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hui::concepts
{

struct NasGame
{
    std::string entry;    // the folder or file name on the NAS (the game's key)
    bool image = false;   // an .exfat image (otherwise a game folder)
    std::string title;    // from param.json, else the entry's name
    std::string title_id; // "PPSA01234", or "" when unknown
    std::string version;  // "01.000.000", or ""
    std::uint64_t size = 0; // bytes: the image, or everything in the folder
    bool size_known = false;
    std::string base;       // folder games: the sce_sys folder, under the games folder
    std::string image_path; // image games: the .exfat file, under the games folder
    // Image games, once their sce_sys was found inside the image:
    bool inside = false;
    ExfatGeometry geometry;
    ExfatFile cover_file; // icon0.png (or pic0.png standing in); empty name: none
    ExfatFile art_file;   // pic0.png; empty name: none
    std::string note;     // why details or the cover are missing ("" when all is well)

    // For the cache: what the NAS said about the game when it was read, so a
    // later check can tell whether it changed.
    std::string stamp;        // its sce_sys files (folders), or the image file
    std::string folder_stamp; // the game folder itself (for the folder size)
    bool cover_saved = false; // its cover is in the cache
    bool art_saved = false;   // its key art is in the cache
};

struct LibraryEvent
{
    enum class Kind
    {
        listed,   // `games` holds every game, sorted by title
        failed,   // the NAS couldn't be read: headline and detail say why
        cover,    // `image` is the cover of the game called `entry`; `tone` its colours
        note,     // the game called `entry` has a problem: `detail` says why
        size,     // the game called `entry` is `bytes` big
        progress, // `done` of `total` steps; `detail` says what is happening
        ready,    // the list, covers and key art are all in: the library is complete
        key_art,  // `image` is the key art asked for with start_key_art()
        finished, // nothing more is coming
    };
    Kind kind = Kind::finished;
    std::vector<NasGame> games;
    std::string headline;
    std::string detail;
    std::string entry;
    std::uint64_t bytes = 0;
    int done = 0;
    int total = 0;
    Image image;
    float tone[3][3] = {}; // dark, mid, accent (RGB 0..1), from the cover
};

// Pure helpers, for tests.
// The values param.json gives (title in the default language).
void read_param_json(const std::string &json, NasGame *game);
// Dark, mid and accent colours of a cover.
void cover_tones(const Image &image, float tone[3][3]);

class LibraryLoader
{
  public:
    LibraryLoader() = default;
    ~LibraryLoader();
    LibraryLoader(const LibraryLoader &) = delete;
    LibraryLoader &operator=(const LibraryLoader &) = delete;

    // Starts reading the library: `saved` is what the cache held (its covers
    // come first, from the PS5's storage), then the NAS is checked. Abandons
    // any earlier load.
    void start(const Connection &connection, const std::vector<NasGame> &saved);
    // Starts fetching one game's key art (sce_sys/pic0.png): from the cache
    // when it's there, else from the NAS. A key_art event brings it.
    void start_key_art(const Connection &connection, const NasGame &game);
    // Abandons the current load (its remaining events are dropped).
    void stop();
    bool active() const
    {
        return job_ != nullptr;
    }
    // The next event, if one is ready. Call from the UI thread.
    bool take(LibraryEvent *event);

  private:
    struct Job;
    Job *job_ = nullptr;
};

} // namespace hui::concepts
