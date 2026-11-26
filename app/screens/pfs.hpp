// NASBridge - Reading a few files out of a PFS image (.ffpfs, and the outer
// image of a .ffpfsc) without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PFS is the PlayStation file system. The unsigned images ShadowMountPlus
// mounts (MkPFS and Dump Forge write them) keep every file in one contiguous
// run of blocks, so a file is found by walking the directories from "uroot"
// and read with a few range requests, as for an .exfat image (exfat.hpp).
//
// A file can be stored compressed: its bytes are then a PFSC container, 64 KiB
// blocks each zlib-compressed (or stored raw when that saves nothing), with a
// table of where each block starts. PfscStream reads such a file at any
// offset, inflating only the blocks a read touches. A .ffpfsc is a PFS image
// holding one such compressed file: another game image (.exfat, .ffpkg or
// .ffpfs).
//
// Read-only, and every number from the image is checked before it is used:
// the image is someone's file on a NAS, not trusted input.

#pragma once

#include "concepts/exfat.hpp" // ExfatReader::ReadAt

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hui::concepts
{

using ReadAt = ExfatReader::ReadAt;

// Reads a PFSC container (a compressed file's bytes) as the file it holds.
class PfscStream
{
  public:
    // The container is `stored` bytes at `at` in what `read` reads; it holds
    // `length` bytes.
    PfscStream(ReadAt read, std::uint64_t at, std::uint64_t stored, std::uint64_t length)
        : read_(std::move(read)), at_(at), stored_(stored), length_(length)
    {
    }

    // Checks the container's header and the ends of its block table.
    bool open(std::string *why);
    std::uint64_t length() const
    {
        return length_;
    }
    // Exactly `length` bytes at `offset` of the file it holds, or false.
    bool read(std::uint64_t offset, std::size_t length, std::string *out, std::string *why);

  private:
    bool entry(std::uint64_t block, std::uint64_t *offset, std::string *why);
    bool block(std::uint64_t index, const std::string **data, std::string *why);

    ReadAt read_;
    std::uint64_t at_;
    std::uint64_t stored_;
    std::uint64_t length_;
    std::uint64_t blocks_ = 0;
    std::uint64_t table_ = 0; // the block table, from at_
    // A window of the block table, so neighbouring blocks share a read.
    std::uint64_t table_window_start_ = 0;
    std::string table_window_;
    // The blocks decoded last, most recent last.
    std::vector<std::pair<std::uint64_t, std::string>> cache_;
};

// One file or folder in a PFS image.
struct PfsFile
{
    std::string name;
    bool directory = false;
    std::uint32_t inode = 0;
};

class PfsReader
{
  public:
    explicit PfsReader(ReadAt read) : read_(std::move(read))
    {
    }

    // Reads the header and finds the root folder ("uroot").
    bool open(std::string *why);

    bool list_root(std::vector<PfsFile> *entries, std::string *why);
    bool list(const PfsFile &folder, std::vector<PfsFile> *entries, std::string *why);
    // A whole file, decompressed (refused when longer than max_length).
    bool read(const PfsFile &file, std::size_t max_length, std::string *data, std::string *why);
    // A way to read a file at any offset (decompressed), and its length: for
    // the image inside a .ffpfsc.
    bool open_file(const PfsFile &file, ReadAt *read, std::uint64_t *length, std::string *why);

    // The entries of the game's sce_sys folder: in the root, or one folder
    // down.
    bool find_sce_sys(std::vector<PfsFile> *entries, std::string *why);

  private:
    // Where an inode's bytes are, as the inode says.
    struct Extent
    {
        std::uint64_t at = 0;     // byte offset in the image
        std::uint64_t stored = 0; // bytes there
        std::uint64_t length = 0; // bytes of the file (more than stored when compressed)
        bool compressed = false;
        bool directory = false;
    };
    bool extent(std::uint32_t inode, Extent *out, std::string *why);
    bool entries(const Extent &folder, std::vector<PfsFile> *out, std::string *why);

    ReadAt read_;
    std::uint32_t block_size_ = 0;
    std::uint64_t inodes_ = 0;
    std::uint64_t blocks_ = 0; // the image's length in blocks
    std::uint32_t uroot_ = 0;
    // The inode table block read last.
    std::uint64_t inode_block_ = ~0ull;
    std::string inode_data_;
};

const PfsFile *find_entry(const std::vector<PfsFile> &entries, const char *name);

} // namespace hui::concepts
