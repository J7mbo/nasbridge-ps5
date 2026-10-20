// NASBridge - Checks that the file functions copying needs exist.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/file_calls.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Weak here, so a missing one reads as 0 instead of being assumed present.
#pragma weak lseek
#pragma weak pread
#pragma weak pwrite
#pragma weak ftruncate
#pragma weak fsync
#pragma weak rename
#pragma weak mkdir
#pragma weak rmdir
#pragma weak unlink

namespace hui::concepts
{

namespace
{

// The address as the loader filled it in; volatile, so the compiler can't
// assume a declared function is there.
template <typename F> bool present(F *function)
{
    F *volatile address = function;
    return address != nullptr;
}

} // namespace

bool file_calls_available(std::string *missing)
{
    missing->clear();
    const auto need = [&](bool there, const char *name)
    {
        if (!there)
            *missing += (missing->empty() ? "" : ", ") + std::string(name);
    };
    need(present(&::lseek), "lseek");
    need(present(&::pread), "pread");
    need(present(&::pwrite), "pwrite");
    need(present(&::ftruncate), "ftruncate");
    need(present(&::fsync), "fsync");
    need(present(&::rename), "rename");
    need(present(&::mkdir), "mkdir");
    need(present(&::rmdir), "rmdir");
    need(present(&::unlink), "unlink");
    return missing->empty();
}

} // namespace hui::concepts
