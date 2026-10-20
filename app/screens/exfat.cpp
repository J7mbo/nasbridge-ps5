// NASBridge - Reading a few files out of an exFAT image without copying it.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout facts used here (Microsoft's exFAT specification):
//   boot sector: "EXFAT   " at 3; FatOffset (sectors) at 80, ClusterHeapOffset
//     at 88, ClusterCount at 92, FirstClusterOfRootDirectory at 96,
//     BytesPerSectorShift at 108, SectorsPerClusterShift at 109.
//   the allocation table (FAT): one 32-bit entry per cluster; 0xFFFFFFFF ends
//     a chain. Cluster 2 is the first in the heap.
//   directories: 32-byte entries. A file is a set: File (0x85), Stream
//     Extension (0xC0: flags, name length, first cluster, data length), then
//     File Name entries (0xC1: 15 UTF-16 characters each). 0x00 ends a folder.

#include "concepts/exfat.hpp"

#include <cstring>

namespace hui::concepts
{

namespace
{

constexpr std::size_t kFatWindow = 64 * 1024;          // allocation table read at once
constexpr std::size_t kMaxFolder = 4u * 1024u * 1024u; // a folder's entries, at most
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

bool is_exfat(const std::string &sector)
{
    return sector.size() >= 512 && std::memcmp(sector.data() + 3, "EXFAT   ", 8) == 0;
}

void append_utf8(std::string *out, std::uint32_t code)
{
    if (code < 0x80)
    {
        *out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        *out += static_cast<char>(0xc0 | (code >> 6));
        *out += static_cast<char>(0x80 | (code & 0x3f));
    }
    else if (code < 0x10000)
    {
        *out += static_cast<char>(0xe0 | (code >> 12));
        *out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        *out += static_cast<char>(0x80 | (code & 0x3f));
    }
    else
    {
        *out += static_cast<char>(0xf0 | (code >> 18));
        *out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        *out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        *out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

const ExfatFile *find_entry(const std::vector<ExfatFile> &entries, const char *name)
{
    const std::size_t n = std::strlen(name);
    for (const ExfatFile &entry : entries)
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

bool ExfatReader::open(std::string *why)
{
    std::string sector;
    if (!read_(0, 512, &sector))
    {
        *why = "couldn't read the start of the image";
        return false;
    }
    std::uint64_t base = 0;
    if (!is_exfat(sector))
    {
        // A disk image with a partition table: try its partitions.
        const unsigned char *p = bytes(sector);
        if (p[510] != 0x55 || p[511] != 0xaa)
        {
            *why = "the image isn't an exFAT image";
            return false;
        }
        bool found = false;
        for (int i = 0; i < 4 && !found; ++i)
        {
            const unsigned char *entry = p + 0x1be + i * 16;
            const unsigned type = entry[4];
            std::uint64_t start = static_cast<std::uint64_t>(u32(entry + 8)) * 512u;
            if (type == 0)
                continue;
            if (type == 0xee)
            {
                // GPT: the first partition entry's starting sector.
                std::string header;
                std::string first;
                if (!read_(512, 512, &header) || std::memcmp(header.data(), "EFI PART", 8) != 0)
                    continue;
                const std::uint64_t entries = u64(bytes(header, 72)) * 512u;
                if (!read_(entries, 128, &first))
                    continue;
                start = u64(bytes(first, 32)) * 512u;
            }
            std::string candidate;
            if (start != 0 && read_(start, 512, &candidate) && is_exfat(candidate))
            {
                base = start;
                sector = candidate;
                found = true;
            }
        }
        if (!found)
        {
            *why = "the image holds no exFAT file system";
            return false;
        }
    }

    const unsigned char *b = bytes(sector);
    const unsigned sector_shift = b[108];
    const unsigned cluster_shift = b[109];
    if (sector_shift < 9 || sector_shift > 12 || sector_shift + cluster_shift > 25)
    {
        *why = "the image's exFAT boot sector is damaged";
        return false;
    }
    ExfatGeometry g;
    g.base = base;
    g.sector_size = 1u << sector_shift;
    g.cluster_size = 1u << (sector_shift + cluster_shift);
    g.fat_offset = static_cast<std::uint64_t>(u32(b + 80)) * g.sector_size;
    g.heap_offset = static_cast<std::uint64_t>(u32(b + 88)) * g.sector_size;
    g.cluster_count = u32(b + 92);
    g.root_cluster = u32(b + 96);
    if (g.cluster_count == 0 || g.root_cluster < 2 ||
        g.root_cluster > g.cluster_count + 1 || g.heap_offset <= g.fat_offset)
    {
        *why = "the image's exFAT boot sector is damaged";
        return false;
    }
    geometry_ = g;
    fat_window_.clear();
    return true;
}

bool ExfatReader::next_cluster(std::uint32_t cluster, std::uint32_t *next, std::string *why)
{
    const std::uint64_t at = geometry_.base + geometry_.fat_offset +
                             static_cast<std::uint64_t>(cluster) * 4u;
    if (fat_window_.empty() || at < fat_window_start_ ||
        at + 4 > fat_window_start_ + fat_window_.size())
    {
        // The table is read in windows, aligned so neighbours share one.
        const std::uint64_t start = at - (at - geometry_.base - geometry_.fat_offset) % kFatWindow;
        const std::uint64_t table_end = geometry_.base + geometry_.fat_offset +
                                        (static_cast<std::uint64_t>(geometry_.cluster_count) + 2) * 4u;
        std::size_t length = kFatWindow;
        if (start + length > table_end)
            length = static_cast<std::size_t>(table_end - start);
        if (length < 4 || !read_(start, length, &fat_window_))
        {
            fat_window_.clear();
            *why = "couldn't read the image's allocation table";
            return false;
        }
        fat_window_start_ = start;
    }
    *next = u32(bytes(fat_window_, static_cast<std::size_t>(at - fat_window_start_)));
    return true;
}

bool ExfatReader::read_chain(std::uint32_t first, std::uint64_t length, bool contiguous,
                             std::size_t max_length, std::string *data, std::string *why)
{
    data->clear();
    const ExfatGeometry &g = geometry_;
    const std::uint64_t last_cluster = static_cast<std::uint64_t>(g.cluster_count) + 1;
    const auto offset_of = [&g](std::uint64_t cluster)
    { return g.base + g.heap_offset + (cluster - 2) * g.cluster_size; };
    if (length > max_length)
    {
        *why = "a file in the image is larger than expected";
        return false;
    }
    if (first < 2 || first > last_cluster)
    {
        if (length == 0 && first == 0)
            return true; // an empty file
        *why = "the image points outside itself";
        return false;
    }

    // length 0 means "until the chain ends" (the root folder), capped.
    const bool open_ended = length == 0;
    const std::uint64_t wanted = open_ended ? max_length : length;
    const std::uint64_t clusters_needed = (wanted + g.cluster_size - 1) / g.cluster_size;

    if (contiguous)
    {
        if (first + clusters_needed - 1 > last_cluster)
        {
            *why = "the image points outside itself";
            return false;
        }
        if (!read_(offset_of(first), static_cast<std::size_t>(wanted), data))
        {
            *why = "couldn't read from the image";
            return false;
        }
        return true;
    }

    // Follow the chain, reading runs of neighbouring clusters in one go.
    std::uint32_t cluster = first;
    std::uint64_t walked = 0; // clusters read so far
    while (walked < clusters_needed)
    {
        const std::uint32_t run_start = cluster;
        std::uint64_t run = 1;
        bool ended = false;
        bool jumped = false;
        std::uint32_t jump = 0;
        while (walked + run < clusters_needed)
        {
            std::uint32_t next = 0;
            if (!next_cluster(cluster, &next, why))
                return false;
            if (next == 0xffffffffu)
            {
                ended = true;
                break;
            }
            if (next < 2 || next > last_cluster)
            {
                *why = "the image's allocation table is damaged";
                return false;
            }
            if (next != cluster + 1)
            {
                jumped = true;
                jump = next;
                break;
            }
            cluster = next;
            ++run;
        }
        const std::uint64_t remaining = wanted - data->size();
        const std::uint64_t run_bytes = run * g.cluster_size;
        const std::size_t take =
            static_cast<std::size_t>(run_bytes < remaining ? run_bytes : remaining);
        std::string part;
        if (!read_(offset_of(run_start), take, &part))
        {
            *why = "couldn't read from the image";
            return false;
        }
        data->append(part);
        walked += run;
        if (ended || !jumped)
            break;
        cluster = jump;
    }
    if (!open_ended && data->size() < length)
    {
        *why = "a file in the image ends early";
        return false;
    }
    return true;
}

bool ExfatReader::parse_entries(const std::string &data, std::vector<ExfatFile> *entries,
                                std::string *why) const
{
    entries->clear();
    const std::size_t count = data.size() / 32;
    for (std::size_t i = 0; i < count; ++i)
    {
        const unsigned char *e = bytes(data, i * 32);
        const unsigned type = e[0];
        if (type == 0x00)
            break; // end of the folder
        if (type != 0x85)
            continue; // unused, deleted, or not a file (bitmap, up-case table, label)
        const unsigned secondary = e[1];
        const bool directory = (u16(e + 4) & 0x10) != 0;
        if (secondary < 2 || i + secondary >= count)
        {
            i += secondary;
            continue;
        }
        const unsigned char *stream = bytes(data, (i + 1) * 32);
        if (stream[0] != 0xc0)
        {
            i += secondary;
            continue;
        }
        ExfatFile file;
        file.directory = directory;
        file.contiguous = (stream[1] & 0x02) != 0;
        const unsigned name_length = stream[3];
        file.first_cluster = u32(stream + 20);
        file.length = u64(stream + 24);
        // The name: 15 UTF-16 code units per File Name entry.
        std::u16string units;
        for (unsigned n = 2; n <= secondary && units.size() < name_length; ++n)
        {
            const unsigned char *part = bytes(data, (i + n) * 32);
            if (part[0] != 0xc1)
                break;
            for (int k = 0; k < 15 && units.size() < name_length; ++k)
                units.push_back(static_cast<char16_t>(u16(part + 2 + k * 2)));
        }
        for (std::size_t k = 0; k < units.size(); ++k)
        {
            std::uint32_t code = units[k];
            if (code >= 0xd800 && code < 0xdc00 && k + 1 < units.size() && units[k + 1] >= 0xdc00 &&
                units[k + 1] < 0xe000)
            {
                code = 0x10000 + ((code - 0xd800) << 10) + (units[k + 1] - 0xdc00);
                ++k;
            }
            append_utf8(&file.name, code);
        }
        i += secondary;
        if (file.name.empty())
            continue;
        entries->push_back(std::move(file));
        if (static_cast<int>(entries->size()) > kMaxFolderEntries)
        {
            *why = "a folder in the image is too big";
            return false;
        }
    }
    return true;
}

bool ExfatReader::list_root(std::vector<ExfatFile> *entries, std::string *why)
{
    std::string data;
    if (!read_chain(geometry_.root_cluster, 0, false, kMaxFolder, &data, why))
        return false;
    return parse_entries(data, entries, why);
}

bool ExfatReader::list(const ExfatFile &folder, std::vector<ExfatFile> *entries, std::string *why)
{
    std::string data;
    if (!read_chain(folder.first_cluster, folder.length, folder.contiguous, kMaxFolder, &data, why))
        return false;
    return parse_entries(data, entries, why);
}

bool ExfatReader::read(const ExfatFile &file, std::size_t max_length, std::string *data,
                       std::string *why)
{
    if (file.length == 0)
    {
        data->clear();
        return true;
    }
    return read_chain(file.first_cluster, file.length, file.contiguous, max_length, data, why);
}

bool ExfatReader::find_sce_sys(std::vector<ExfatFile> *entries, std::string *why)
{
    std::vector<ExfatFile> root;
    if (!list_root(&root, why))
        return false;
    const ExfatFile *sce_sys = find_entry(root, "sce_sys");
    if (sce_sys != nullptr && sce_sys->directory)
        return list(*sce_sys, entries, why);
    int tried = 0;
    for (const ExfatFile &folder : root)
    {
        if (!folder.directory || tried++ >= 4)
            continue;
        std::vector<ExfatFile> inside;
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
