// NASBridge - The saved library: the list, covers and key art, on the PS5.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kept in the app's own storage (/download0/nasbridge/cache), which only
// this app can reach:
//   library.dat         the list (in the kit's checked save container), for
//                       one connection: another NAS or folder starts afresh
//   covers/<key>.png    each game's cover, as the NAS sent it
//   art/<key>.png       each game's key art (pic0.png), as the NAS sent it
// <key> is a hash of the game's name on the NAS.

#pragma once

#include "concepts/connection.hpp"
#include "concepts/nas_library.hpp"

#include <string>
#include <vector>

namespace hui::concepts
{

// The saved list for this connection; false (and empty) when there is none.
bool load_library(const Connection &connection, std::vector<NasGame> *games);
// Saves the list; returns an error, or "".
std::string save_library(const Connection &connection, const std::vector<NasGame> &games);

std::string cover_path(const std::string &entry);
std::string art_path(const std::string &entry);
// Writes a picture file of the cache; false when it couldn't (storage full).
bool save_picture(const std::string &path, const std::string &data);
// Reads one (at most max_bytes); false when it isn't there.
bool load_picture(const std::string &path, std::string *data, std::size_t max_bytes);
// Removes the pictures of games that are gone.
void forget_pictures(const std::string &entry);

// NASBridge's own folder on the PS5 (/download0/nasbridge), made if needed.
std::string data_root();

// For tests: keep the cache somewhere else.
void set_cache_root(const std::string &root);
// For tests: the list as text and back.
std::string library_to_text(const Connection &connection, const std::vector<NasGame> &games);
bool library_from_text(const Connection &connection, const std::string &text,
                       std::vector<NasGame> *games);

} // namespace hui::concepts
