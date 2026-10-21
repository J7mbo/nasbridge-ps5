// NASBridge - Reading a few files out of an exFAT image without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A game image (.exfat) is a whole exFAT file system in one file. To show its
// name and cover, only a handful of small pieces are needed: the boot sector,
// a few directory clusters, parts of the allocation table, and the files in
// sce_sys. Each piece is read with an HTTP Range request (see ReadAt), so a
// 50 GB image costs a few hundred kilobytes.
//
// Read-only, and every number from the image is checked before it is used:
// the image is someone's file on a NAS, not trusted input.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::concepts
{

// Where things are in one image (from its boot sector).
struct ExfatGeometry
{
    std::uint64_t base = 0;        // where the file system starts in the image file
    std::uint32_t sector_size = 0; // bytes
    std::uint32_t cluster_size = 0;
    std::uint64_t fat_offset = 0;  // bytes, from base
    std::uint64_t heap_offset = 0; // bytes, from base: cluster 2
    std::uint32_t cluster_count = 0;
    std::uint32_t root_cluster = 0;
};

// One file or folder inside the image.
struct ExfatFile
{
    std::string name;
    bool directory = false;
    std::uint32_t first_cluster = 0;
    std::uint64_t length = 0;
    bool contiguous = false; // "NoFatChain": its clusters follow one another
};

class ExfatReader
{
  public:
    // Reads `length` bytes at `offset` of the image file into *out (exactly
    // that many, or returns false).
    using ReadAt = std::function<bool(std::uint64_t offset, std::size_t length, std::string *out)>;

    explicit ExfatReader(ReadAt read) : read_(std::move(read))
    {
    }

    // How big a folder may be (bytes of entries, and entries): a cover needs
    // little, checking a whole image more.
    void set_limits(std::size_t max_folder_bytes, int max_folder_entries)
    {
        max_folder_bytes_ = max_folder_bytes;
        max_folder_entries_ = max_folder_entries;
    }

    // Reads the boot sector (at the start, or in the first MBR partition).
    bool open(std::string *why);
    const ExfatGeometry &geometry() const
    {
        return geometry_;
    }
    // For a reader whose geometry is already known (no boot sector read).
    void set_geometry(const ExfatGeometry &geometry)
    {
        geometry_ = geometry;
    }

    // The entries of the root folder, or of a folder found in it.
    bool list_root(std::vector<ExfatFile> *entries, std::string *why);
    bool list(const ExfatFile &folder, std::vector<ExfatFile> *entries, std::string *why);
    // A whole file (refused when longer than max_length).
    bool read(const ExfatFile &file, std::size_t max_length, std::string *data, std::string *why);

    // The entries of the game's sce_sys folder: in the root, or one folder
    // down (an image holding "PPSA01234-app/sce_sys").
    bool find_sce_sys(std::vector<ExfatFile> *entries, std::string *why);

  private:
    bool read_chain(std::uint32_t first, std::uint64_t length, bool contiguous,
                    std::size_t max_length, std::string *data, std::string *why);
    bool next_cluster(std::uint32_t cluster, std::uint32_t *next, std::string *why);
    bool parse_entries(const std::string &data, std::vector<ExfatFile> *entries,
                       std::string *why) const;

    ReadAt read_;
    ExfatGeometry geometry_;
    // A window of the allocation table, so following a chain doesn't cost
    // one request per cluster.
    std::uint64_t fat_window_start_ = 0;
    std::string fat_window_;
    std::size_t max_folder_bytes_ = 4u * 1024u * 1024u;
    int max_folder_entries_ = 4096;
};

// Case-insensitive (ASCII) lookup in a folder listing; nullptr when absent.
const ExfatFile *find_entry(const std::vector<ExfatFile> &entries, const char *name);

} // namespace hui::concepts
