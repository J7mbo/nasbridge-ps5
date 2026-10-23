// NASBridge - Laying out a folder game as an exFAT image, to copy it as one.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Copy as an exFAT image" writes a folder game from the NAS straight into one
// .exfat file that ShadowMountPlus mounts, with no second pass and no room
// needed for the folder as well. That works because the whole image can be
// laid out from the file list alone, before a byte of the game is read: every
// file gets one contiguous run of clusters, in the order the copy fetches
// them, so the image is written strictly front to back, as every other copy
// is, and a resumed copy carries on from the image's length.
//
// The image, by offset (512-byte sectors, 64 KiB clusters, the layout
// ShadowMountPlus's own exfat scripts and PS5 Dump Forge make):
//   sectors 0..11     the boot region (boot sector, 8 extended boot sectors,
//                     OEM parameters, reserved, checksum)
//   sectors 12..23    its backup
//   sector 128        the allocation table (FAT), padded to a cluster
//   cluster 2..       allocation bitmap, up-case table, root folder
//   then              the game: a folder, then everything in it
//   last              free clusters (0.5% of the game, 64 MiB to 512 MiB), so
//                     a game mounted writable has room for small changes
// Every timestamp is the same and the serial number comes from the file list,
// so the same game always makes the same bytes: a resumed copy lays it out
// again rather than saving it.

#pragma once

#include "concepts/exfat.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hui::concepts
{

// One file of the game: its path in the game ("sce_sys/param.json") and size.
struct ImageFile
{
    std::string path;
    std::uint64_t size = 0;
};

// A run of the image, in order from its start.
struct ImagePiece
{
    enum class Kind
    {
        bytes, // `data`: the file system's own bytes
        zeros,
        file,  // file number `file`, then nothing: its padding is the next piece
    };
    Kind kind = Kind::zeros;
    std::uint64_t length = 0;
    std::string data;
    std::size_t file = 0;
};

struct ExfatImage
{
    std::uint64_t size = 0;       // bytes: the pieces' lengths add up to it
    std::uint64_t free_bytes = 0; // room left inside it
    std::vector<ImagePiece> pieces;
};

// Lays the game out. `folders` (parents before children or not) and the
// files' paths are relative to the game; `label` is the volume's name (the
// title ID; at most 11 characters, or left out). False with *why when the game
// can't be an exFAT image: a name exFAT can't hold, or two names in a folder
// that differ only in case.
bool plan_exfat_image(const std::vector<std::string> &folders, const std::vector<ImageFile> &files,
                      const std::string &label, ExfatImage *image, std::string *why);

// An image's exact size, worked out as a game's listing streams in, without
// keeping the list: the size plan_exfat_image lays the same game out at.
// Each folder and file is added once, in any order (a file's folders needn't
// come first), with its path relative to the game. size() is 0 when the game
// can't be an image (a name exFAT can't hold, a folder too full, too big).
// Two names differing only in case aren't caught here: the copy finds those.
class ExfatImageSize
{
  public:
    void add_folder(const std::string &path);
    void add_file(const std::string &path, std::uint64_t size);
    std::uint64_t size() const;

  private:
    bool folder(const std::string &path);
    bool name_entries(const std::string &name, std::uint64_t *entries);

    std::unordered_map<std::string, std::uint64_t> folders_; // each folder: its children's entries
    std::uint64_t file_clusters_ = 0;
    bool bad_ = false;
};

// Reads an image back and checks it holds exactly these folders and files,
// each file at its size, laid out where the image has room. False with *why.
bool check_exfat_image(const ExfatReader::ReadAt &read, const std::vector<std::string> &folders,
                       const std::vector<ImageFile> &files, std::string *why);

} // namespace hui::concepts
