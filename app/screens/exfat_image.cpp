// NASBridge - Laying out a folder game as an exFAT image, to copy it as one.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Written from Microsoft's exFAT specification; the sizes and the order of
// things follow PS5 Dump Forge's image writer, which ShadowMountPlus mounts.
// Fields used (offsets in bytes):
//   boot sector: JumpBoot 0, "EXFAT   " 3, VolumeLength 72, FatOffset 80,
//     FatLength 84, ClusterHeapOffset 88, ClusterCount 92, root folder's
//     cluster 96, VolumeSerialNumber 100, FileSystemRevision 104,
//     BytesPerSectorShift 108, SectorsPerClusterShift 109, NumberOfFats 110,
//     DriveSelect 111, PercentInUse 112, signature 0x55AA at 510.
//   folder entries (32 bytes): File 0x85, Stream Extension 0xC0, File Name
//     0xC1, Allocation Bitmap 0x81, Up-case Table 0x82, Volume Label 0x83.

#include "concepts/exfat_image.hpp"

#include "concepts/exfat_upcase.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

namespace hui::concepts
{

namespace
{

constexpr std::uint64_t kSector = 512;
constexpr unsigned kSectorShift = 9;
constexpr unsigned kClusterShift = 7; // 128 sectors: 64 KiB clusters
constexpr std::uint64_t kSectorsPerCluster = 1u << kClusterShift;
constexpr std::uint64_t kCluster = kSector * kSectorsPerCluster;
constexpr std::uint64_t kFatOffset = kSectorsPerCluster; // sectors: the FAT starts a cluster in
constexpr std::uint64_t kMaxClusters = 0xFFFFFFF5ull;    // ClusterCount's limit
constexpr std::uint64_t kMaxFolderBytes = 256u << 20;    // a folder's DataLength's limit
constexpr std::uint64_t kSpareMin = 64u << 20;
constexpr std::uint64_t kSpareMax = 512u << 20;
constexpr std::size_t kUpcaseBytes = sizeof(kExfatUpcase);
// 2024-01-01 00:00:00 on every file: the image depends on the file list alone.
constexpr std::uint32_t kTimestamp = ((2024u - 1980u) << 25) | (1u << 21) | (1u << 16);

void put16(std::string *s, std::size_t at, std::uint16_t v)
{
    (*s)[at] = static_cast<char>(v & 0xff);
    (*s)[at + 1] = static_cast<char>(v >> 8);
}

void put32(std::string *s, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        (*s)[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

void put64(std::string *s, std::size_t at, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        (*s)[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t unit)
{
    return (value + unit - 1) / unit * unit;
}

// UTF-8 to UTF-16; false when it isn't UTF-8.
bool to_utf16(const std::string &text, std::u16string *out)
{
    out->clear();
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0;
        int extra = 0;
        if (c < 0x80)
            code = c;
        else if ((c & 0xe0) == 0xc0)
            code = c & 0x1f, extra = 1;
        else if ((c & 0xf0) == 0xe0)
            code = c & 0x0f, extra = 2;
        else if ((c & 0xf8) == 0xf0)
            code = c & 0x07, extra = 3;
        else
            return false;
        if (extra > 0 && i + static_cast<std::size_t>(extra) >= text.size())
            return false;
        for (int k = 1; k <= extra; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
            (extra == 3 && (code < 0x10000 || code > 0x10ffff)) || (code >= 0xd800 && code < 0xe000))
            return false;
        if (code >= 0x10000)
        {
            code -= 0x10000;
            out->push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            out->push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        }
        else
        {
            out->push_back(static_cast<char16_t>(code));
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

// The up-case table, one entry per UTF-16 unit (what the image's table says).
const std::vector<std::uint16_t> &upcase_map()
{
    static const std::vector<std::uint16_t> map = []
    {
        std::vector<std::uint16_t> m(0x10000);
        for (std::size_t i = 0; i < m.size(); ++i)
            m[i] = static_cast<std::uint16_t>(i);
        const std::size_t n = sizeof(kExfatUpcase) / sizeof(kExfatUpcase[0]);
        std::size_t at = 0;
        for (std::size_t i = 0; i < n && at < m.size();)
        {
            if (kExfatUpcase[i] == 0xffff && i + 1 < n)
            {
                at += kExfatUpcase[i + 1];
                i += 2;
            }
            else
            {
                m[at++] = kExfatUpcase[i++];
            }
        }
        return m;
    }();
    return map;
}

std::u16string upcased(const std::u16string &units)
{
    const std::vector<std::uint16_t> &map = upcase_map();
    std::u16string out = units;
    for (char16_t &u : out)
        u = static_cast<char16_t>(map[u]);
    return out;
}

// A name exFAT can hold (spec 7.7.3): 1..255 UTF-16 units, no control
// characters, none of " * / : < > ? \ |, and not "." or "..".
std::string name_problem(const std::string &name, const std::u16string &units)
{
    if (name.empty() || name == "." || name == "..")
        return "isn't a name a file can have";
    if (units.size() > 255)
        return "is longer than exFAT allows";
    for (const char16_t u : units)
        if (u < 0x20 || u == '"' || u == '*' || u == '/' || u == ':' || u == '<' || u == '>' ||
            u == '?' || u == '\\' || u == '|')
            return "has a character exFAT doesn't allow";
    return "";
}

std::uint16_t name_hash(const std::u16string &upper)
{
    std::uint16_t hash = 0;
    for (const char16_t unit : upper)
        for (int b = 0; b < 2; ++b)
        {
            const std::uint8_t byte = static_cast<std::uint8_t>(b == 0 ? unit & 0xff : unit >> 8);
            hash = static_cast<std::uint16_t>(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + byte);
        }
    return hash;
}

std::uint32_t upcase_checksum()
{
    std::uint32_t sum = 0;
    for (const std::uint16_t unit : kExfatUpcase)
        for (int b = 0; b < 2; ++b)
        {
            const std::uint8_t byte = static_cast<std::uint8_t>(b == 0 ? unit & 0xff : unit >> 8);
            sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + byte;
        }
    return sum;
}

struct Node
{
    std::string name; // UTF-8, as on the NAS
    std::u16string units;
    bool directory = false;
    std::uint64_t length = 0; // a file's size; a folder's entries, padded to clusters
    std::size_t file = 0;     // a file: which
    std::vector<std::size_t> children;
    std::uint32_t first_cluster = 0;
    std::uint64_t clusters = 0;
};

struct Tree
{
    std::vector<Node> nodes; // [0] is the root folder
    std::unordered_map<std::string, std::size_t> by_path;
    std::string why;

    Tree()
    {
        nodes.emplace_back();
        nodes[0].directory = true;
        by_path[""] = 0;
    }

    // The node at `path`, made (with its parents) as a folder if new; npos
    // with `why` when a name can't be had.
    std::size_t folder(const std::string &path)
    {
        const auto found = by_path.find(path);
        if (found != by_path.end())
        {
            if (!nodes[found->second].directory)
            {
                why = "\"" + path + "\" is both a file and a folder";
                return std::string::npos;
            }
            return found->second;
        }
        const std::size_t slash = path.rfind('/');
        const std::size_t parent = folder(slash == std::string::npos ? "" : path.substr(0, slash));
        if (parent == std::string::npos)
            return parent;
        return add(parent, path, slash == std::string::npos ? path : path.substr(slash + 1), true);
    }

    std::size_t add(std::size_t parent, const std::string &path, const std::string &name, bool directory)
    {
        Node node;
        node.name = name;
        node.directory = directory;
        if (!to_utf16(name, &node.units))
        {
            why = "\"" + path + "\" has a name that isn't UTF-8";
            return std::string::npos;
        }
        const std::string problem = name_problem(name, node.units);
        if (!problem.empty())
        {
            why = "\"" + path + "\" " + problem;
            return std::string::npos;
        }
        nodes.push_back(std::move(node));
        const std::size_t index = nodes.size() - 1;
        nodes[parent].children.push_back(index);
        by_path[path] = index;
        return index;
    }
};

// Entries a child takes in its folder: File, Stream Extension, and a File
// Name per 15 units.
std::uint64_t entries_for(const Node &node)
{
    return 2 + (node.units.size() + 14) / 15;
}

// A file's or folder's entry set (File, Stream Extension, File Names).
std::string entry_set(const Node &node)
{
    const std::size_t names = (node.units.size() + 14) / 15;
    std::string set(32 * (2 + names), '\0');
    set[0] = static_cast<char>(0x85);
    set[1] = static_cast<char>(1 + names);
    put16(&set, 4, node.directory ? 0x10 : 0x20); // directory : archive
    put32(&set, 8, kTimestamp);                   // created
    put32(&set, 12, kTimestamp);                  // modified
    put32(&set, 16, kTimestamp);                  // accessed
    // Stream Extension: allocation possible; one contiguous run ("no FAT
    // chain"); an empty file has no clusters at all.
    const std::size_t s = 32;
    set[s] = static_cast<char>(0xC0);
    set[s + 1] = static_cast<char>(node.clusters > 0 ? 0x03 : 0x01);
    set[s + 3] = static_cast<char>(node.units.size());
    put16(&set, s + 4, name_hash(upcased(node.units)));
    put64(&set, s + 8, node.length); // ValidDataLength: all of it is written
    put32(&set, s + 20, node.clusters > 0 ? node.first_cluster : 0);
    put64(&set, s + 24, node.length);
    for (std::size_t n = 0; n < names; ++n)
    {
        const std::size_t e = 64 + 32 * n;
        set[e] = static_cast<char>(0xC1);
        for (std::size_t k = 0; k < 15; ++k)
        {
            const std::size_t i = n * 15 + k;
            if (i < node.units.size())
                put16(&set, e + 2 + 2 * k, static_cast<std::uint16_t>(node.units[i]));
        }
    }
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < set.size(); ++i)
    {
        if (i == 2 || i == 3)
            continue;
        sum = static_cast<std::uint16_t>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) +
                                         static_cast<std::uint8_t>(set[i]));
    }
    put16(&set, 2, sum);
    return set;
}

// The pieces, merging neighbours of a kind.
class Pieces
{
  public:
    explicit Pieces(std::vector<ImagePiece> *out) : out_(out)
    {
    }
    void bytes(std::string data)
    {
        if (data.empty())
            return;
        if (!out_->empty() && out_->back().kind == ImagePiece::Kind::bytes)
        {
            out_->back().data += data;
            out_->back().length += data.size();
            return;
        }
        ImagePiece piece;
        piece.kind = ImagePiece::Kind::bytes;
        piece.length = data.size();
        piece.data = std::move(data);
        out_->push_back(std::move(piece));
    }
    void zeros(std::uint64_t length)
    {
        if (length == 0)
            return;
        if (!out_->empty() && out_->back().kind == ImagePiece::Kind::zeros)
        {
            out_->back().length += length;
            return;
        }
        ImagePiece piece;
        piece.kind = ImagePiece::Kind::zeros;
        piece.length = length;
        out_->push_back(std::move(piece));
    }
    void file(std::size_t index, std::uint64_t length)
    {
        if (length == 0)
            return;
        ImagePiece piece;
        piece.kind = ImagePiece::Kind::file;
        piece.length = length;
        piece.file = index;
        out_->push_back(std::move(piece));
    }

  private:
    std::vector<ImagePiece> *out_;
};

// The boot region: boot sector, 8 extended boot sectors, OEM parameters,
// reserved, then the checksum sector.
std::string boot_region(std::uint64_t volume_sectors, std::uint64_t fat_sectors,
                        std::uint64_t heap_sector, std::uint64_t cluster_count, std::uint32_t root,
                        std::uint32_t serial, unsigned percent)
{
    std::string region(12 * kSector, '\0');
    region[0] = static_cast<char>(0xEB);
    region[1] = static_cast<char>(0x76);
    region[2] = static_cast<char>(0x90);
    std::memcpy(&region[3], "EXFAT   ", 8);
    put64(&region, 64, 0); // PartitionOffset
    put64(&region, 72, volume_sectors);
    put32(&region, 80, static_cast<std::uint32_t>(kFatOffset));
    put32(&region, 84, static_cast<std::uint32_t>(fat_sectors));
    put32(&region, 88, static_cast<std::uint32_t>(heap_sector));
    put32(&region, 92, static_cast<std::uint32_t>(cluster_count));
    put32(&region, 96, root);
    put32(&region, 100, serial);
    put16(&region, 104, 0x0100); // revision 1.00
    put16(&region, 106, 0);      // VolumeFlags: clean
    region[108] = static_cast<char>(kSectorShift);
    region[109] = static_cast<char>(kClusterShift);
    region[110] = 1;                       // one FAT
    region[111] = static_cast<char>(0x80); // DriveSelect
    region[112] = static_cast<char>(percent);
    for (std::size_t i = 120; i < 510; ++i)
        region[i] = static_cast<char>(0xF4); // BootCode: halt
    region[510] = 0x55;
    region[511] = static_cast<char>(0xAA);
    for (std::size_t s = 1; s <= 8; ++s) // extended boot sectors' signature
    {
        region[s * kSector + 510] = 0x55;
        region[s * kSector + 511] = static_cast<char>(0xAA);
    }
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < 11 * kSector; ++i)
    {
        if (i == 106 || i == 107 || i == 112)
            continue; // VolumeFlags and PercentInUse change without the checksum
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + static_cast<std::uint8_t>(region[i]);
    }
    for (std::size_t at = 11 * kSector; at < 12 * kSector; at += 4)
        put32(&region, at, sum);
    return region;
}

} // namespace

bool plan_exfat_image(const std::vector<std::string> &folders, const std::vector<ImageFile> &files,
                      const std::string &label, ExfatImage *image, std::string *why)
{
    *image = ExfatImage{};
    std::u16string label_units;
    if (!to_utf16(label, &label_units) || label_units.size() > 11)
        label_units.clear();

    // 1. The tree.
    Tree tree;
    for (const std::string &folder : folders)
    {
        if (!folder.empty() && tree.folder(folder) == std::string::npos)
        {
            *why = tree.why;
            return false;
        }
    }
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const std::string &path = files[i].path;
        if (tree.by_path.count(path) != 0)
        {
            *why = "\"" + path + "\" is listed twice";
            return false;
        }
        const std::size_t slash = path.rfind('/');
        const std::size_t parent = tree.folder(slash == std::string::npos ? "" : path.substr(0, slash));
        const std::size_t node =
            parent == std::string::npos
                ? parent
                : tree.add(parent, path, slash == std::string::npos ? path : path.substr(slash + 1), false);
        if (node == std::string::npos)
        {
            *why = tree.why;
            return false;
        }
        tree.nodes[node].length = files[i].size;
        tree.nodes[node].file = i;
    }
    std::vector<Node> &nodes = tree.nodes;

    // 2. Each folder: its children in name order, no two the same to exFAT,
    //    and how long its entries are.
    for (std::size_t n = 0; n < nodes.size(); ++n)
    {
        Node &dir = nodes[n];
        if (!dir.directory)
            continue;
        std::sort(dir.children.begin(), dir.children.end(),
                  [&nodes](std::size_t a, std::size_t b) { return nodes[a].name < nodes[b].name; });
        std::map<std::u16string, std::size_t> seen;
        std::uint64_t entries = n == 0 ? 3 : 0; // the root: label, bitmap, up-case table
        for (const std::size_t child : dir.children)
        {
            const auto [at, fresh] = seen.emplace(upcased(nodes[child].units), child);
            if (!fresh)
            {
                *why = "\"" + nodes[at->second].name + "\" and \"" + nodes[child].name +
                       "\" differ only in upper and lower case, which one exFAT folder can't hold";
                return false;
            }
            entries += entries_for(nodes[child]);
        }
        dir.length = std::max<std::uint64_t>(round_up(entries * 32, kCluster), kCluster);
        if (dir.length > kMaxFolderBytes)
        {
            *why = "a folder of the game holds more files than exFAT allows";
            return false;
        }
    }

    // 3. Where everything goes: bitmap, up-case table, root folder, then each
    //    folder followed by what's in it.
    std::vector<std::size_t> order; // nodes in the heap, in order (root first)
    std::uint64_t game_clusters = 0;
    const auto take = [&](std::size_t n)
    {
        nodes[n].clusters = (nodes[n].length + kCluster - 1) / kCluster;
        game_clusters += nodes[n].clusters;
        order.push_back(n);
    };
    take(0);
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}}; // folder, next child
    while (!stack.empty())
    {
        auto &[dir, next] = stack.back();
        if (next >= nodes[dir].children.size())
        {
            stack.pop_back();
            continue;
        }
        const std::size_t child = nodes[dir].children[next++];
        take(child);
        if (nodes[child].directory)
            stack.emplace_back(child, 0);
    }
    const std::uint64_t upcase_clusters = 1; // 5,836 bytes
    std::uint64_t bitmap_clusters = 1, cluster_count = 0, spare = 0;
    for (int pass = 0; pass < 8; ++pass)
    {
        const std::uint64_t used = bitmap_clusters + upcase_clusters + game_clusters;
        const std::uint64_t spare_bytes = std::clamp<std::uint64_t>(used * kCluster / 200, kSpareMin, kSpareMax);
        spare = spare_bytes / kCluster;
        cluster_count = used + spare;
        const std::uint64_t need = round_up((cluster_count + 7) / 8, kCluster) / kCluster;
        if (need == bitmap_clusters)
            break;
        bitmap_clusters = need;
    }
    if (cluster_count > kMaxClusters)
    {
        *why = "the game is too big for one exFAT image";
        return false;
    }
    const std::uint64_t used = cluster_count - spare;
    std::uint64_t next_cluster = 2;
    const std::uint32_t bitmap_first = static_cast<std::uint32_t>(next_cluster);
    next_cluster += bitmap_clusters;
    const std::uint32_t upcase_first = static_cast<std::uint32_t>(next_cluster);
    next_cluster += upcase_clusters;
    for (const std::size_t n : order)
    {
        nodes[n].first_cluster = static_cast<std::uint32_t>(next_cluster);
        next_cluster += nodes[n].clusters;
    }
    const std::uint64_t fat_sectors =
        round_up(((cluster_count + 2) * 4 + kSector - 1) / kSector, kSectorsPerCluster);
    const std::uint64_t heap_sector = kFatOffset + fat_sectors;
    const std::uint64_t volume_sectors = heap_sector + cluster_count * kSectorsPerCluster;

    // 4. The bytes.
    std::uint32_t serial = 2166136261u; // FNV-1a over the list
    for (const std::size_t n : order)
    {
        const std::string key = nodes[n].name + "/" + std::to_string(nodes[n].length) + "/";
        for (const char c : key)
            serial = (serial ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    const unsigned percent = static_cast<unsigned>(used * 100 / cluster_count);
    const std::string boot = boot_region(volume_sectors, fat_sectors, heap_sector, cluster_count,
                                         nodes[0].first_cluster, serial, percent);

    Pieces pieces(&image->pieces);
    {
        std::string start(kFatOffset * kSector, '\0');
        std::memcpy(&start[0], boot.data(), boot.size());
        std::memcpy(&start[boot.size()], boot.data(), boot.size()); // the backup
        pieces.bytes(std::move(start));
    }
    {
        // The FAT: an explicit chain for every run (a driver that ignores "no
        // FAT chain" reads the same clusters).
        std::string fat(fat_sectors * kSector, '\0');
        put32(&fat, 0, 0xFFFFFFF8u); // media type
        put32(&fat, 4, 0xFFFFFFFFu);
        const auto chain = [&fat](std::uint64_t first, std::uint64_t count)
        {
            for (std::uint64_t c = first; c < first + count; ++c)
                put32(&fat, static_cast<std::size_t>(c * 4),
                      c + 1 < first + count ? static_cast<std::uint32_t>(c + 1) : 0xFFFFFFFFu);
        };
        chain(bitmap_first, bitmap_clusters);
        chain(upcase_first, upcase_clusters);
        for (const std::size_t n : order)
            chain(nodes[n].first_cluster, nodes[n].clusters);
        pieces.bytes(std::move(fat));
    }
    {
        // The allocation bitmap: everything up to the spare is in use.
        std::string bitmap(bitmap_clusters * kCluster, '\0');
        for (std::uint64_t c = 0; c < used; ++c)
            bitmap[static_cast<std::size_t>(c / 8)] |= static_cast<char>(1u << (c % 8));
        pieces.bytes(std::move(bitmap));
        std::string upcase(upcase_clusters * kCluster, '\0');
        for (std::size_t i = 0; i < sizeof(kExfatUpcase) / sizeof(kExfatUpcase[0]); ++i)
            put16(&upcase, 2 * i, kExfatUpcase[i]);
        pieces.bytes(std::move(upcase));
    }
    for (const std::size_t n : order)
    {
        const Node &node = nodes[n];
        if (!node.directory)
        {
            pieces.file(node.file, node.length);
            pieces.zeros(node.clusters * kCluster - node.length);
            continue;
        }
        std::string entries;
        if (n == 0)
        {
            std::string head(96, '\0');
            head[0] = static_cast<char>(0x83); // Volume Label
            head[1] = static_cast<char>(label_units.size());
            for (std::size_t i = 0; i < label_units.size(); ++i)
                put16(&head, 2 + 2 * i, static_cast<std::uint16_t>(label_units[i]));
            head[32] = static_cast<char>(0x81); // Allocation Bitmap
            put32(&head, 32 + 20, bitmap_first);
            put64(&head, 32 + 24, (cluster_count + 7) / 8);
            head[64] = static_cast<char>(0x82); // Up-case Table
            put32(&head, 64 + 4, upcase_checksum());
            put32(&head, 64 + 20, upcase_first);
            put64(&head, 64 + 24, kUpcaseBytes);
            entries = std::move(head);
        }
        for (const std::size_t child : node.children)
            entries += entry_set(nodes[child]);
        entries.resize(static_cast<std::size_t>(node.length), '\0');
        pieces.bytes(std::move(entries));
    }
    pieces.zeros(spare * kCluster);

    image->size = volume_sectors * kSector;
    image->free_bytes = spare * kCluster;
    return true;
}

bool check_exfat_image(const ExfatReader::ReadAt &read, const std::vector<std::string> &folders,
                       const std::vector<ImageFile> &files, std::string *why)
{
    ExfatReader reader(read);
    reader.set_limits(kMaxFolderBytes, 1 << 22);
    if (!reader.open(why))
        return false;
    const ExfatGeometry &g = reader.geometry();
    const std::uint64_t last_cluster = static_cast<std::uint64_t>(g.cluster_count) + 1;

    // What should be there: path -> size (folders: ~0).
    constexpr std::uint64_t kFolder = ~0ull;
    std::unordered_map<std::string, std::uint64_t> expected;
    for (const ImageFile &file : files)
    {
        expected[file.path] = file.size;
        for (std::size_t slash = file.path.find('/'); slash != std::string::npos;
             slash = file.path.find('/', slash + 1))
            expected[file.path.substr(0, slash)] = kFolder;
    }
    for (const std::string &folder : folders)
        if (!folder.empty())
            expected[folder] = kFolder;

    std::size_t found = 0;
    std::vector<std::pair<std::string, ExfatFile>> pending; // folders still to list
    std::vector<ExfatFile> entries;
    std::string base; // the folder being looked at, with its slash ("" at the root)
    if (!reader.list_root(&entries, why))
        return false;
    for (;;)
    {
        for (const ExfatFile &entry : entries)
        {
            const std::string path = base + entry.name;
            const auto want = expected.find(path);
            if (want == expected.end())
            {
                *why = "the image holds \"" + path + "\", which the game doesn't";
                return false;
            }
            if ((want->second == kFolder) != entry.directory ||
                (!entry.directory && want->second != entry.length))
            {
                *why = "\"" + path + "\" isn't as it should be in the image";
                return false;
            }
            const std::uint64_t clusters = (entry.length + g.cluster_size - 1) / g.cluster_size;
            if (clusters > 0 && (entry.first_cluster < 2 || entry.first_cluster + clusters - 1 > last_cluster))
            {
                *why = "\"" + path + "\" lies outside the image";
                return false;
            }
            ++found;
            if (entry.directory)
                pending.emplace_back(path, entry);
        }
        if (pending.empty())
            break;
        const ExfatFile folder = pending.back().second;
        base = pending.back().first + "/";
        pending.pop_back();
        if (!reader.list(folder, &entries, why))
            return false;
    }
    if (found != expected.size())
    {
        *why = "the image is missing some of the game's files";
        return false;
    }
    return true;
}

} // namespace hui::concepts
