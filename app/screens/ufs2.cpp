// NASBridge - Reading a few files out of a UFS2 image without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout facts used here (FreeBSD's sys/ufs/ffs/fs.h, ufs/dinode.h and
// ufs/dir.h):
//   superblock at 64 KiB: inode table offset in a cylinder group (fragments)
//     at 16, group count at 44, block size at 48, fragment size at 52,
//     inodes per group at 184, fragments per group at 188, size in
//     fragments (64-bit) at 1080, magic 0x19540119 at 1372.
//   inodes: 256 bytes, inode n at group n / ipg, slot n % ipg of its table.
//     Mode at 0 (0x4000 folder, 0x8000 file), size at 16, 12 direct block
//     addresses at 112, single/double/triple indirect at 208; all addresses
//     are in fragments, and 0 is a hole.
//   folders: records of inode (4), record length (2), type (1), name
//     length (1) and the name; inode 0 is an unused record. The root is
//     inode 2.

#include "concepts/ufs2.hpp"

#include <cstring>

namespace hui::concepts
{

namespace
{

constexpr std::uint64_t kSuperblock = 65536;
constexpr std::uint32_t kMagic = 0x19540119;
constexpr std::uint64_t kInodeSize = 256;
constexpr std::uint64_t kRoot = 2;
constexpr std::uint16_t kTypeMask = 0xf000;
constexpr std::uint16_t kDirectory = 0x4000;
constexpr std::uint16_t kFile = 0x8000;
constexpr std::size_t kMaxFolder = 4u * 1024u * 1024u; // a folder's records, at most
constexpr int kMaxFolderEntries = 4096;

std::uint16_t u16(const unsigned char *p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t u32(const unsigned char *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t u64(const unsigned char *p)
{
    return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4)) << 32);
}

const unsigned char *bytes(const std::string &s, std::size_t at = 0)
{
    return reinterpret_cast<const unsigned char *>(s.data()) + at;
}

bool power_of_two(std::uint32_t n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

const Ufs2File *find_entry(const std::vector<Ufs2File> &entries, const char *name)
{
    const std::size_t n = std::strlen(name);
    for (const Ufs2File &entry : entries)
    {
        if (entry.name.size() != n)
            continue;
        bool same = true;
        for (std::size_t i = 0; i < n && same; ++i)
            same = lower(entry.name[i]) == lower(name[i]);
        if (same)
            return &entry;
    }
    return nullptr;
}

bool Ufs2Reader::open(std::string *why)
{
    std::string sb;
    if (!read_(kSuperblock, 1376, &sb))
    {
        *why = "couldn't read the image's superblock";
        return false;
    }
    const unsigned char *s = bytes(sb);
    if (u32(s + 1372) != kMagic)
    {
        *why = "the image isn't a UFS2 image";
        return false;
    }
    inode_table_ = u32(s + 16);
    groups_ = u32(s + 44);
    block_size_ = u32(s + 48);
    fragment_size_ = u32(s + 52);
    inodes_per_group_ = u32(s + 184);
    fragments_per_group_ = u32(s + 188);
    fragments_ = u64(s + 1080);
    const bool sizes = power_of_two(fragment_size_) && fragment_size_ >= 512 &&
                       fragment_size_ <= 65536 && power_of_two(block_size_) &&
                       block_size_ >= fragment_size_ && block_size_ / fragment_size_ <= 8;
    if (!sizes || groups_ == 0 || inodes_per_group_ == 0 || fragments_per_group_ == 0 ||
        static_cast<std::uint64_t>(inode_table_) * fragment_size_ +
                static_cast<std::uint64_t>(inodes_per_group_) * kInodeSize >
            static_cast<std::uint64_t>(fragments_per_group_) * fragment_size_)
    {
        *why = "the image's UFS2 superblock is damaged";
        return false;
    }
    indirect_.clear();
    return true;
}

bool Ufs2Reader::inode(std::uint64_t number, Inode *out, std::string *why)
{
    if (number == 0 || number >= static_cast<std::uint64_t>(groups_) * inodes_per_group_)
    {
        *why = "the image points outside itself";
        return false;
    }
    const std::uint64_t group = number / inodes_per_group_;
    const std::uint64_t at = (group * fragments_per_group_ + inode_table_) * fragment_size_ +
                             (number % inodes_per_group_) * kInodeSize;
    std::string data;
    if (!read_(at, kInodeSize, &data))
    {
        *why = "couldn't read the image's inodes";
        return false;
    }
    const unsigned char *d = bytes(data);
    Inode node;
    node.mode = u16(d);
    node.size = u64(d + 16);
    for (int i = 0; i < 12; ++i)
        node.direct[i] = u64(d + 112 + i * 8);
    for (int i = 0; i < 3; ++i)
        node.indirect[i] = u64(d + 208 + i * 8);
    *out = node;
    return true;
}

bool Ufs2Reader::pointer(const Inode &node, std::uint64_t index, std::uint64_t *fragment,
                         std::string *why)
{
    if (index < 12)
    {
        *fragment = node.direct[index];
        return true;
    }
    index -= 12;
    const std::uint64_t per = block_size_ / 8;
    std::uint64_t span = per;
    for (int level = 0; level < 3; ++level)
    {
        if (index >= span)
        {
            index -= span;
            span *= per;
            continue;
        }
        std::uint64_t block = node.indirect[level];
        for (int depth = level; depth >= 0; --depth)
        {
            if (block == 0)
            {
                *fragment = 0; // a hole
                return true;
            }
            if (block >= fragments_)
            {
                *why = "the image points outside itself";
                return false;
            }
            const std::uint64_t at = block * fragment_size_;
            if (indirect_.empty() || indirect_at_ != at)
            {
                if (!read_(at, block_size_, &indirect_))
                {
                    indirect_.clear();
                    *why = "couldn't read from the image";
                    return false;
                }
                indirect_at_ = at;
            }
            std::uint64_t below = 1;
            for (int k = 0; k < depth; ++k)
                below *= per;
            block = u64(bytes(indirect_, static_cast<std::size_t>((index / below) % per) * 8));
        }
        *fragment = block;
        return true;
    }
    *why = "a file in the image is larger than expected";
    return false;
}

bool Ufs2Reader::contents(const Inode &node, std::size_t max_length, std::string *data,
                          std::string *why)
{
    data->clear();
    if (node.size > max_length)
    {
        *why = "a file in the image is larger than expected";
        return false;
    }
    const std::uint64_t blocks = (node.size + block_size_ - 1) / block_size_;
    std::uint64_t index = 0;
    while (index < blocks)
    {
        // Blocks that follow one another on disk are read in one go.
        std::uint64_t first = 0;
        if (!pointer(node, index, &first, why))
            return false;
        std::uint64_t run = 1;
        const std::uint64_t step = block_size_ / fragment_size_;
        while (first != 0 && index + run < blocks)
        {
            std::uint64_t next = 0;
            if (!pointer(node, index + run, &next, why))
                return false;
            if (next != first + run * step)
                break;
            ++run;
        }
        const std::uint64_t left = node.size - data->size();
        const std::uint64_t want = run * block_size_ < left ? run * block_size_ : left;
        if (first == 0)
        {
            data->append(static_cast<std::size_t>(want), '\0');
        }
        else
        {
            if (first >= fragments_ || (want + fragment_size_ - 1) / fragment_size_ > fragments_ - first)
            {
                *why = "the image points outside itself";
                return false;
            }
            std::string part;
            if (!read_(first * fragment_size_, static_cast<std::size_t>(want), &part))
            {
                *why = "couldn't read from the image";
                return false;
            }
            data->append(part);
        }
        index += run;
    }
    return true;
}

bool Ufs2Reader::list(const Ufs2File &folder, std::vector<Ufs2File> *entries, std::string *why)
{
    entries->clear();
    Inode node;
    if (!inode(folder.inode, &node, why))
        return false;
    if ((node.mode & kTypeMask) != kDirectory)
    {
        *why = "the image is damaged (a folder that isn't one)";
        return false;
    }
    std::string data;
    if (!contents(node, kMaxFolder, &data, why))
        return false;
    std::size_t at = 0;
    while (at + 8 <= data.size())
    {
        const unsigned char *e = bytes(data, at);
        const std::uint32_t number = u32(e);
        const std::uint16_t length = u16(e + 4);
        const unsigned name_length = e[7];
        if (length < 8 || length % 4 != 0 || length > data.size() - at ||
            (number != 0 && name_length + 8u > length))
        {
            *why = "a folder of the image is damaged";
            return false;
        }
        at += length;
        if (number == 0 || name_length == 0)
            continue;
        Ufs2File file;
        file.name.assign(data, at - length + 8, name_length);
        if (file.name == "." || file.name == "..")
            continue;
        if (file.name.find('\0') != std::string::npos || file.name.find('/') != std::string::npos)
        {
            *why = "a folder of the image is damaged";
            return false;
        }
        file.inode = number;
        file.directory = e[6] == 4; // DT_DIR
        entries->push_back(std::move(file));
        if (static_cast<int>(entries->size()) > kMaxFolderEntries)
        {
            *why = "a folder in the image is too big";
            return false;
        }
    }
    return true;
}

bool Ufs2Reader::list_root(std::vector<Ufs2File> *entries, std::string *why)
{
    Ufs2File root;
    root.directory = true;
    root.inode = kRoot;
    return list(root, entries, why);
}

bool Ufs2Reader::read(const Ufs2File &file, std::size_t max_length, std::string *data,
                      std::string *why)
{
    Inode node;
    if (!inode(file.inode, &node, why))
        return false;
    if ((node.mode & kTypeMask) != kFile)
    {
        *why = "the image is damaged (a file that isn't one)";
        return false;
    }
    return contents(node, max_length, data, why);
}

bool Ufs2Reader::find_sce_sys(std::vector<Ufs2File> *entries, std::string *why)
{
    std::vector<Ufs2File> root;
    if (!list_root(&root, why))
        return false;
    const Ufs2File *sce_sys = find_entry(root, "sce_sys");
    if (sce_sys != nullptr && sce_sys->directory)
        return list(*sce_sys, entries, why);
    int tried = 0;
    for (const Ufs2File &folder : root)
    {
        if (!folder.directory || tried++ >= 4)
            continue;
        std::vector<Ufs2File> inside;
        if (!list(folder, &inside, why))
            continue;
        sce_sys = find_entry(inside, "sce_sys");
        if (sce_sys != nullptr && sce_sys->directory)
            return list(*sce_sys, entries, why);
    }
    *why = "the image has no sce_sys folder";
    return false;
}

} // namespace hui::concepts
