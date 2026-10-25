// NASBridge - Looking inside a compressed game image (.ffpfsc).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layout follows what ShadowMountPlus mounts and MkPFS (PSBrew) and Dump
// Forge (quer3q) write, both GPL-3.0: the outer image's root holds the
// compressed image (named for its format, or pfs_image.dat for a PFS image).
// The format inside is told by its contents, not its name.

#include "concepts/ffpfsc.hpp"

#include <cstring>

namespace hui::concepts
{

namespace
{

bool ends_with_nocase(const std::string &text, const char *suffix)
{
    const std::size_t n = std::strlen(suffix);
    if (text.size() < n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
    {
        char a = text[text.size() - n + i];
        char b = suffix[i];
        a = a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a;
        b = b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b;
        if (a != b)
            return false;
    }
    return true;
}

bool same_nocase(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && ends_with_nocase(a, b.c_str());
}

// The compressed images ShadowMountPlus looks for inside a .ffpfsc, first.
bool image_name(const std::string &name)
{
    return ends_with_nocase(name, ".exfat") || ends_with_nocase(name, ".ffpkg") ||
           ends_with_nocase(name, ".ffpfs") || same_nocase(name, "pfs_image.dat");
}

} // namespace

bool FfpfscReader::open(std::string *why)
{
    PfsReader outer(read_);
    if (!outer.open(why))
    {
        *why = "the compressed image's outer PFS image: " + *why;
        return false;
    }
    return open_container(outer, 0, why);
}

bool FfpfscReader::open_container(PfsReader &container, int depth, std::string *why)
{
    std::vector<PfsFile> root;
    if (!container.list_root(&root, why))
        return false;
    std::vector<const PfsFile *> candidates;
    for (const PfsFile &file : root)
        if (!file.directory && image_name(file.name))
            candidates.push_back(&file);
    for (const PfsFile &file : root)
        if (!file.directory && !image_name(file.name))
            candidates.push_back(&file);
    if (candidates.empty())
    {
        *why = "the compressed image holds no game image";
        return false;
    }
    std::string first_why;
    for (const PfsFile *file : candidates)
    {
        ReadAt inner;
        std::uint64_t length = 0;
        std::string problem;
        if (container.open_file(*file, &inner, &length, &problem) &&
            open_inner(inner, depth, &problem))
            return true;
        if (first_why.empty())
            first_why = file->name + ": " + problem;
    }
    *why = first_why;
    return false;
}

bool FfpfscReader::open_inner(const ReadAt &inner, int depth, std::string *why)
{
    exfat_.reset();
    ufs2_.reset();
    pfs_.reset();
    names_.clear();
    // Told apart by their first bytes: exFAT's name at 3, PFS's magic at 8,
    // else UFS2 (its superblock is 64 KiB in).
    std::string start;
    if (!inner(0, 512, &start))
    {
        *why = "couldn't read the start of the image inside";
        return false;
    }
    const unsigned char *s = reinterpret_cast<const unsigned char *>(start.data());
    const bool exfat = std::memcmp(s + 3, "EXFAT   ", 8) == 0 || (s[510] == 0x55 && s[511] == 0xaa);
    std::uint64_t magic = 0;
    for (int i = 7; i >= 0; --i)
        magic = (magic << 8) | s[8 + i];
    if (magic == 20130315)
    {
        format_ = "PFS";
        auto pfs = std::make_unique<PfsReader>(inner);
        if (!pfs->open(why))
            return false;
        if (!pfs->find_sce_sys(&pfs_files_, why))
        {
            // No game in it: another compressed container (MkPFS 1.1's
            // "pack folder" makes an exFAT image inside one, nested once).
            std::string nested;
            if (depth < 1 && open_container(*pfs, depth + 1, &nested))
                return true;
            return false;
        }
        pfs_ = std::move(pfs);
        for (const PfsFile &file : pfs_files_)
            if (!file.directory)
                names_.push_back(file.name);
        return true;
    }
    if (exfat)
    {
        format_ = "exFAT";
        exfat_ = std::make_unique<ExfatReader>(inner);
        if (!exfat_->open(why) || !exfat_->find_sce_sys(&exfat_files_, why))
            return false;
        for (const ExfatFile &file : exfat_files_)
            if (!file.directory)
                names_.push_back(file.name);
        return true;
    }
    format_ = "UFS2";
    ufs2_ = std::make_unique<Ufs2Reader>(inner);
    if (!ufs2_->open(why))
    {
        *why = "the image inside is neither exFAT, UFS2 nor PFS";
        return false;
    }
    if (!ufs2_->find_sce_sys(&ufs2_files_, why))
        return false;
    for (const Ufs2File &file : ufs2_files_)
        if (!file.directory)
            names_.push_back(file.name);
    return true;
}

bool FfpfscReader::read(const std::string &name, std::size_t max_length, std::string *data,
                        std::string *why)
{
    if (exfat_)
    {
        const ExfatFile *file = find_entry(exfat_files_, name.c_str());
        if (file != nullptr && !file->directory)
            return exfat_->read(*file, max_length, data, why);
    }
    else if (ufs2_)
    {
        const Ufs2File *file = find_entry(ufs2_files_, name.c_str());
        if (file != nullptr && !file->directory)
            return ufs2_->read(*file, max_length, data, why);
    }
    else if (pfs_)
    {
        const PfsFile *file = find_entry(pfs_files_, name.c_str());
        if (file != nullptr && !file->directory)
            return pfs_->read(*file, max_length, data, why);
    }
    *why = "the image's sce_sys has no " + name;
    return false;
}

} // namespace hui::concepts
