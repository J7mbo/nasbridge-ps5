// NASBridge - Deleting a game through the delete helper payload.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// NASBridge runs in the game sandbox, and ShadowMountPlus refuses to delete
// anything while NASBridge is open (it counts NASBridge as a running game).
// So a delete sends the helper payload (tools/helper/helper.c, shipped as
// assets/helper.elf) to the PS5's own ELF loader, which runs it with the
// jailbreak's privileges; the helper deletes the game's files, removes it from
// the home screen, reports back and exits. Blocks: call from a worker thread.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hui::concepts
{

struct DeleteResult
{
    bool ok = false;
    // On failure: "loader" (the ELF loader didn't answer), "helper" (the
    // helper didn't start or stopped without answering), "request" (it refused
    // before touching anything), "files" or "uninstall" (where it stopped).
    std::string stage;
    std::string why;
};

// Deletes `path` (the game's folder or image as ShadowMountPlus reports it;
// "" for an installed PKG, which only needs uninstalling) and uninstalls
// `title_id`. progress(done, total) is called as files go, from this thread.
DeleteResult delete_with_helper(const std::string &title_id, const std::string &path,
                                const std::function<void(std::uint64_t, std::uint64_t)> &progress);

// For a folder game ShadowMountPlus has already removed from the home screen
// (it also releases a game's attached files; the helper can't): renames the
// folder `path` into <its parent>/.nasbridge-deleting (ShadowMountPlus skips
// names starting with a dot), then deletes that hidden folder, `threads`
// files at a time: this game and anything an earlier delete left there.
// Its progress counts files, not bytes (each file takes about as long).
// threads 0: delete_with_helper.
DeleteResult delete_files_with_helper(const std::string &title_id, const std::string &path,
                                      const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                                      int threads);

// Deletes what earlier deletes left in <root>/.nasbridge-deleting (root: a
// folder games go to, "/data/homebrew"). Nothing there is fine.
DeleteResult finish_deletes_with_helper(const std::string &root,
                                        const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                                        int threads);

// Tests: the helper file, the loader's port and the helper's port.
void set_delete_helper(const std::string &elf_path, int loader_port, int helper_port);

} // namespace hui::concepts
