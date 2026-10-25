// NASBridge - Reading a few files out of a PFS image without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout facts used here (the unsigned PS5 images MkPFS writes; MkPFS by
// PSBrew and Dump Forge by quer3q, both GPL-3.0, were read for the format):
//   header (block 0): version (1 or 2) at 0, magic 20130315 at 8, mode at
//     0x1C (signed 0x1, 64-bit inodes 0x2, encrypted 0x4: none are read),
//     block size at 0x20, inode count at 0x30, image length in blocks at 0x38.
//   inodes (from block 1): 0xA8 bytes each, never across a block. Mode at 0
//     (0x4000 folder, 0x8000 file), flags at 4 (0x1 compressed), size at 8,
//     "compressed size" at 0x10 (for a compressed file: the size of the file
//     it holds; size is then the container's), block count at 0x60, 12
//     direct block numbers at 0x64, 5 indirect ones at 0x94.
//   folders: entries of inode (4), type (4: 2 file, 3 folder, 4 ".",
//     5 ".."), name length (4), entry length (4), then the name. Inode 0 is
//     the super-root, whose "uroot" folder is the root of the game's files.
//   PFSC (a compressed file): magic "PFSC" at 0, block size at 0xC and 0x10
//     (64 KiB), block table at 0x18 (from the container's start), data at
//     0x20, padded length at 0x28. The table holds one start per block and
//     the end: a block as long as the block size is stored raw, a shorter one
//     is a zlib stream of it.

#include "concepts/pfs.hpp"

#include "third_party/stb_image/stb_image.h" // its zlib decoder

#include <cstring>
#include <memory>

namespace hui::concepts
{

namespace
{

constexpr std::uint64_t kPfsMagic = 20130315;
constexpr std::size_t kInodeSize = 0xA8;
constexpr std::uint16_t kModeDirectory = 0x4000;
constexpr std::uint16_t kModeFile = 0x8000;
constexpr std::uint32_t kFlagCompressed = 0x1;
constexpr std::int32_t kTypeFile = 2;
constexpr std::int32_t kTypeDirectory = 3;
constexpr std::uint64_t kMaxInodes = 4000000;
constexpr std::size_t kMaxFolder = 4u * 1024u * 1024u; // a folder's entries, at most
constexpr int kMaxFolderEntries = 4096;

constexpr std::uint32_t kPfscMagic = 0x43534650; // "PFSC"
constexpr std::uint64_t kPfscBlock = 0x10000;
constexpr std::size_t kPfscHeader = 0x30;
constexpr std::size_t kTableWindow = 4096;  // 512 block starts read at once
constexpr std::size_t kCachedBlocks = 8;    // decoded blocks kept

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

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

const PfsFile *find_entry(const std::vector<PfsFile> &entries, const char *name)
{
    const std::size_t n = std::strlen(name);
    for (const PfsFile &entry : entries)
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

// ---- PFSC ----

bool PfscStream::open(std::string *why)
{
    std::string header;
    if (stored_ < kPfscHeader || !read_(at_, kPfscHeader, &header))
    {
        *why = "couldn't read the compressed image's header";
        return false;
    }
    const unsigned char *h = bytes(header);
    const std::uint64_t data = u64(h + 0x20);
    blocks_ = (length_ + kPfscBlock - 1) / kPfscBlock;
    table_ = u64(h + 0x18);
    if (u32(h) != kPfscMagic || u32(h + 0xc) != kPfscBlock || u64(h + 0x10) != kPfscBlock)
    {
        *why = "the compressed image isn't a PFSC container with 64 KiB blocks";
        return false;
    }
    if (u64(h + 0x28) < blocks_ * kPfscBlock || table_ < kPfscHeader ||
        table_ > stored_ || (stored_ - table_) / 8 < blocks_ + 1 || table_ + (blocks_ + 1) * 8 > data ||
        data > stored_)
    {
        *why = "the compressed image's block table doesn't fit it";
        return false;
    }
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (!entry(0, &first, why) || !entry(blocks_, &last, why))
        return false;
    if (first != data || last != stored_)
    {
        *why = "the compressed image's block table doesn't span it";
        return false;
    }
    return true;
}

bool PfscStream::entry(std::uint64_t block, std::uint64_t *offset, std::string *why)
{
    const std::uint64_t at = table_ + block * 8;
    if (table_window_.empty() || at < table_window_start_ ||
        at + 8 > table_window_start_ + table_window_.size())
    {
        const std::uint64_t start = at - (at - table_) % kTableWindow;
        const std::uint64_t end = table_ + (blocks_ + 1) * 8;
        const std::size_t length =
            static_cast<std::size_t>(end - start < kTableWindow ? end - start : kTableWindow);
        if (!read_(at_ + start, length, &table_window_))
        {
            table_window_.clear();
            *why = "couldn't read the compressed image's block table";
            return false;
        }
        table_window_start_ = start;
    }
    *offset = u64(bytes(table_window_, static_cast<std::size_t>(at - table_window_start_)));
    return true;
}

bool PfscStream::block(std::uint64_t index, const std::string **data, std::string *why)
{
    for (std::size_t i = 0; i < cache_.size(); ++i)
    {
        if (cache_[i].first != index)
            continue;
        if (i + 1 != cache_.size())
        {
            auto hit = std::move(cache_[i]);
            cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(i));
            cache_.push_back(std::move(hit));
        }
        *data = &cache_.back().second;
        return true;
    }
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if (!entry(index, &start, why) || !entry(index + 1, &end, why))
        return false;
    if (end <= start || end - start > kPfscBlock || end > stored_)
    {
        *why = "the compressed image's block table is damaged";
        return false;
    }
    std::string stored;
    if (!read_(at_ + start, static_cast<std::size_t>(end - start), &stored))
    {
        *why = "couldn't read from the compressed image";
        return false;
    }
    std::string plain;
    if (stored.size() == kPfscBlock)
    {
        plain = std::move(stored);
    }
    else
    {
        // At most one block comes out: a stream that wants more fails.
        plain.resize(kPfscBlock);
        const int got = stbi_zlib_decode_buffer(plain.data(), static_cast<int>(kPfscBlock),
                                                stored.data(), static_cast<int>(stored.size()));
        if (got != static_cast<int>(kPfscBlock))
        {
            *why = "a block of the compressed image doesn't decompress";
            return false;
        }
    }
    if (cache_.size() >= kCachedBlocks)
        cache_.erase(cache_.begin());
    cache_.emplace_back(index, std::move(plain));
    *data = &cache_.back().second;
    return true;
}

bool PfscStream::read(std::uint64_t offset, std::size_t length, std::string *out, std::string *why)
{
    out->clear();
    if (offset > length_ || length > length_ - offset)
    {
        *why = "a read goes past the end of the compressed image";
        return false;
    }
    out->reserve(length);
    std::uint64_t at = offset;
    while (out->size() < length)
    {
        const std::string *data = nullptr;
        if (!block(at / kPfscBlock, &data, why))
            return false;
        const std::size_t within = static_cast<std::size_t>(at % kPfscBlock);
        std::size_t take = static_cast<std::size_t>(kPfscBlock) - within;
        if (take > length - out->size())
            take = length - out->size();
        out->append(*data, within, take);
        at += take;
    }
    return true;
}

// ---- PFS ----

bool PfsReader::open(std::string *why)
{
    std::string header;
    if (!read_(0, 0x400, &header))
    {
        *why = "couldn't read the start of the image";
        return false;
    }
    const unsigned char *h = bytes(header);
    const std::uint64_t version = u64(h);
    if ((version != 1 && version != 2) || u64(h + 8) != kPfsMagic)
    {
        *why = "the image isn't a PFS image";
        return false;
    }
    const std::uint16_t mode = u16(h + 0x1c);
    if ((mode & 0x1) != 0 || (mode & 0x2) != 0 || (mode & 0x4) != 0)
    {
        *why = (mode & 0x4) != 0   ? "the PFS image is encrypted"
               : (mode & 0x1) != 0 ? "signed PFS images aren't supported"
                                   : "PFS images with 64-bit inodes aren't supported";
        return false;
    }
    block_size_ = u32(h + 0x20);
    inodes_ = u64(h + 0x30);
    blocks_ = u64(h + 0x38);
    if (block_size_ < 0x1000 || block_size_ > 0x10000 || (block_size_ & (block_size_ - 1)) != 0)
    {
        *why = "the PFS image's block size is unsupported";
        return false;
    }
    const std::uint64_t per_block = block_size_ / kInodeSize;
    if (inodes_ < 3 || inodes_ > kMaxInodes || 1 + (inodes_ + per_block - 1) / per_block > blocks_)
    {
        *why = "the PFS image's header is damaged";
        return false;
    }
    inode_block_ = ~0ull;
    inode_data_.clear();

    // The super-root (inode 0) lists the root folder, "uroot".
    Extent super_root;
    std::vector<PfsFile> specials;
    if (!extent(0, &super_root, why) || !super_root.directory || !entries(super_root, &specials, why))
    {
        if (why->empty())
            *why = "the PFS image's super-root is damaged";
        return false;
    }
    const PfsFile *uroot = find_entry(specials, "uroot");
    if (uroot == nullptr || !uroot->directory)
    {
        *why = "the PFS image has no root folder";
        return false;
    }
    uroot_ = uroot->inode;
    return true;
}

bool PfsReader::extent(std::uint32_t inode, Extent *out, std::string *why)
{
    if (inode >= inodes_)
    {
        *why = "the PFS image points outside itself";
        return false;
    }
    const std::uint64_t per_block = block_size_ / kInodeSize;
    const std::uint64_t block = 1 + inode / per_block;
    if (block != inode_block_)
    {
        if (!read_(block * block_size_, block_size_, &inode_data_))
        {
            inode_block_ = ~0ull;
            inode_data_.clear();
            *why = "couldn't read the PFS image's inodes";
            return false;
        }
        inode_block_ = block;
    }
    const unsigned char *d = bytes(inode_data_, static_cast<std::size_t>(inode % per_block) * kInodeSize);
    const std::uint16_t mode = u16(d);
    const std::uint32_t flags = u32(d + 4);
    const std::uint64_t size = u64(d + 8);
    const std::uint64_t inner = u64(d + 0x10);
    const std::uint64_t count = u32(d + 0x60);
    const std::int64_t first = static_cast<std::int32_t>(u32(d + 0x64));
    Extent e;
    e.compressed = (flags & kFlagCompressed) != 0;
    e.directory = (mode & kModeDirectory) != 0;
    e.stored = size;
    e.length = e.compressed ? inner : size;
    if ((mode & (kModeDirectory | kModeFile)) == 0)
    {
        *why = "the PFS image has an inode that is neither a file nor a folder";
        return false;
    }
    // One run of blocks from the first direct block: the later direct
    // blocks are -1 (or name the run's next blocks) and no indirect ones.
    const std::uint64_t needed = size == 0 ? 1 : (size + block_size_ - 1) / block_size_;
    bool run = first > 0 && count >= needed && static_cast<std::uint64_t>(first) < blocks_ &&
               needed <= blocks_ - static_cast<std::uint64_t>(first);
    for (int k = 1; k < 12 && run; ++k)
    {
        const std::int64_t next = static_cast<std::int32_t>(u32(d + 0x64 + k * 4));
        run = next == -1 || next == 0 || (static_cast<std::uint64_t>(k) < needed && next == first + k);
    }
    for (int k = 0; k < 5 && run; ++k)
        run = u32(d + 0x94 + k * 4) == 0 || u32(d + 0x94 + k * 4) == 0xffffffffu;
    if (!run)
    {
        *why = "the PFS image stores a file in a way NASBridge can't read";
        return false;
    }
    e.at = static_cast<std::uint64_t>(first) * block_size_;
    *out = e;
    return true;
}

bool PfsReader::entries(const Extent &folder, std::vector<PfsFile> *out, std::string *why)
{
    out->clear();
    if (folder.compressed || folder.stored > kMaxFolder)
    {
        *why = "a folder in the PFS image can't be read";
        return false;
    }
    std::string data;
    if (folder.stored > 0 && !read_(folder.at, static_cast<std::size_t>(folder.stored), &data))
    {
        *why = "couldn't read a folder of the PFS image";
        return false;
    }
    std::size_t at = 0;
    while (at + 16 <= data.size())
    {
        const unsigned char *e = bytes(data, at);
        const std::uint32_t inode = u32(e);
        const std::int32_t type = static_cast<std::int32_t>(u32(e + 4));
        const std::int32_t name_length = static_cast<std::int32_t>(u32(e + 8));
        const std::int32_t length = static_cast<std::int32_t>(u32(e + 12));
        if (inode == 0 && type == 0 && name_length == 0 && length == 0)
        {
            // The end of the entries, or of the ones in this block.
            const std::size_t next = (at / block_size_ + 1) * block_size_;
            if (next + 16 > data.size() || data.compare(next, 16, std::string(16, '\0')) == 0)
                break;
            at = next;
            continue;
        }
        if (name_length <= 0 || name_length > 255 || length % 8 != 0 || length < 16 + name_length ||
            static_cast<std::size_t>(length) > data.size() - at)
        {
            *why = "a folder of the PFS image is damaged";
            return false;
        }
        at += static_cast<std::size_t>(length);
        if (type != kTypeFile && type != kTypeDirectory)
            continue; // "." and ".."
        PfsFile file;
        file.name.assign(data, at - static_cast<std::size_t>(length) + 16,
                         static_cast<std::size_t>(name_length));
        if (file.name.find('\0') != std::string::npos || file.name.find('/') != std::string::npos)
        {
            *why = "a folder of the PFS image is damaged";
            return false;
        }
        file.directory = type == kTypeDirectory;
        file.inode = inode;
        out->push_back(std::move(file));
        if (static_cast<int>(out->size()) > kMaxFolderEntries)
        {
            *why = "a folder in the PFS image is too big";
            return false;
        }
    }
    return true;
}

bool PfsReader::list_root(std::vector<PfsFile> *entries_out, std::string *why)
{
    PfsFile root;
    root.directory = true;
    root.inode = uroot_;
    return list(root, entries_out, why);
}

bool PfsReader::list(const PfsFile &folder, std::vector<PfsFile> *entries_out, std::string *why)
{
    Extent e;
    if (!extent(folder.inode, &e, why))
        return false;
    if (!e.directory)
    {
        *why = "the PFS image is damaged (a folder that isn't one)";
        return false;
    }
    return entries(e, entries_out, why);
}

bool PfsReader::open_file(const PfsFile &file, ReadAt *read, std::uint64_t *length, std::string *why)
{
    Extent e;
    if (!extent(file.inode, &e, why))
        return false;
    if (e.directory)
    {
        *why = "the PFS image is damaged (a file that is a folder)";
        return false;
    }
    *length = e.length;
    if (!e.compressed)
    {
        const ReadAt image = read_;
        const std::uint64_t at = e.at;
        const std::uint64_t size = e.length;
        *read = [image, at, size](std::uint64_t offset, std::size_t n, std::string *out)
        {
            if (offset > size || n > size - offset)
                return false;
            return image(at + offset, n, out);
        };
        return true;
    }
    auto stream = std::make_shared<PfscStream>(read_, e.at, e.stored, e.length);
    if (!stream->open(why))
        return false;
    *read = [stream](std::uint64_t offset, std::size_t n, std::string *out)
    {
        std::string ignored;
        return stream->read(offset, n, out, &ignored);
    };
    return true;
}

bool PfsReader::read(const PfsFile &file, std::size_t max_length, std::string *data, std::string *why)
{
    ReadAt reader;
    std::uint64_t length = 0;
    if (!open_file(file, &reader, &length, why))
        return false;
    if (length > max_length)
    {
        *why = "a file in the image is larger than expected";
        return false;
    }
    data->clear();
    if (length == 0)
        return true;
    if (!reader(0, static_cast<std::size_t>(length), data))
    {
        *why = "couldn't read from the PFS image";
        return false;
    }
    return true;
}

bool PfsReader::find_sce_sys(std::vector<PfsFile> *entries_out, std::string *why)
{
    std::vector<PfsFile> root;
    if (!list_root(&root, why))
        return false;
    const PfsFile *sce_sys = find_entry(root, "sce_sys");
    if (sce_sys != nullptr && sce_sys->directory)
        return list(*sce_sys, entries_out, why);
    int tried = 0;
    for (const PfsFile &folder : root)
    {
        if (!folder.directory || tried++ >= 4)
            continue;
        std::vector<PfsFile> inside;
        if (!list(folder, &inside, why))
            continue;
        sce_sys = find_entry(inside, "sce_sys");
        if (sce_sys != nullptr && sce_sys->directory)
            return list(*sce_sys, entries_out, why);
    }
    *why = "the image has no sce_sys folder";
    return false;
}

} // namespace hui::concepts
