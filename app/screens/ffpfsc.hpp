// NASBridge - Looking inside a compressed game image (.ffpfsc).
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A .ffpfsc is a small PFS image (pfs.hpp) holding one compressed file, and
// that file is another game image: an .exfat (exfat.hpp), a .ffpkg (UFS2,
// ufs2.hpp) or a .ffpfs / pfs_image.dat (PFS again). ShadowMountPlus mounts
// it as it is; the console decompresses it as the game reads.
//
// To show the game in the library only its sce_sys is needed, so this opens
// the layers one after another: the outer image, the compressed file (only
// the 64 KiB blocks a read lands in are fetched and inflated), the image
// inside it, and its sce_sys folder. A game costs a megabyte or two of reads,
// however big the image.

#pragma once

#include "concepts/exfat.hpp"
#include "concepts/pfs.hpp"
#include "concepts/ufs2.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hui::concepts
{

class FfpfscReader
{
  public:
    explicit FfpfscReader(ReadAt read) : read_(std::move(read))
    {
    }

    // Opens every layer down to the game's sce_sys folder.
    bool open(std::string *why);

    // The file names in the game's sce_sys folder (not its folders).
    const std::vector<std::string> &sce_sys() const
    {
        return names_;
    }
    // What the compressed image holds: "exFAT", "UFS2" or "PFS".
    const std::string &inner_format() const
    {
        return format_;
    }
    // A file in sce_sys, by name (any case); refused when longer than
    // max_length.
    bool read(const std::string &name, std::size_t max_length, std::string *data, std::string *why);

  private:
    bool open_container(PfsReader &container, int depth, std::string *why);
    bool open_inner(const ReadAt &inner, int depth, std::string *why);

    ReadAt read_;
    std::string format_;
    std::vector<std::string> names_;
    // The image inside, as whichever reader it needs, and its sce_sys.
    std::unique_ptr<ExfatReader> exfat_;
    std::unique_ptr<Ufs2Reader> ufs2_;
    std::unique_ptr<PfsReader> pfs_;
    std::vector<ExfatFile> exfat_files_;
    std::vector<Ufs2File> ufs2_files_;
    std::vector<PfsFile> pfs_files_;
};

} // namespace hui::concepts
