// NASBridge - Reading a few files out of a UFS2 image (.ffpkg) without
// copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A .ffpkg is a whole UFS2 file system (FreeBSD's) in one file. Inside a
// .ffpfsc it is the most common image; this reader is what finds the game's
// sce_sys in it, the way exfat.hpp does for an .exfat image: the superblock,
// a few inodes and folders, and the files in sce_sys, each with a range read.
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

// One file or folder in a UFS2 image.
struct Ufs2File
{
    std::string name;
    bool directory = false;
    std::uint64_t inode = 0;
};

class Ufs2Reader
{
  public:
    using ReadAt = ExfatReader::ReadAt;

    explicit Ufs2Reader(ReadAt read) : read_(std::move(read))
    {
    }

    // Reads the superblock.
    bool open(std::string *why);

    bool list_root(std::vector<Ufs2File> *entries, std::string *why);
    bool list(const Ufs2File &folder, std::vector<Ufs2File> *entries, std::string *why);
    // A whole file (refused when longer than max_length).
    bool read(const Ufs2File &file, std::size_t max_length, std::string *data, std::string *why);

    // The entries of the game's sce_sys folder: in the root, or one folder
    // down.
    bool find_sce_sys(std::vector<Ufs2File> *entries, std::string *why);

  private:
    struct Inode
    {
        std::uint16_t mode = 0;
        std::uint64_t size = 0;
        std::uint64_t direct[12] = {};
        std::uint64_t indirect[3] = {};
    };
    bool inode(std::uint64_t number, Inode *out, std::string *why);
    bool pointer(const Inode &node, std::uint64_t index, std::uint64_t *fragment, std::string *why);
    bool contents(const Inode &node, std::size_t max_length, std::string *data, std::string *why);

    ReadAt read_;
    std::uint32_t block_size_ = 0;
    std::uint32_t fragment_size_ = 0;
    std::uint64_t fragments_ = 0; // the file system's length in fragments
    std::uint32_t groups_ = 0;
    std::uint32_t inodes_per_group_ = 0;
    std::uint32_t fragments_per_group_ = 0;
    std::uint32_t inode_table_ = 0; // fragment of each group's inodes, in the group
    // The indirect block read last.
    std::uint64_t indirect_at_ = 0;
    std::string indirect_;
};

const Ufs2File *find_entry(const std::vector<Ufs2File> &entries, const char *name);

} // namespace hui::concepts
