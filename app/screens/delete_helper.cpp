// NASBridge - Deleting a game through the delete helper payload.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/delete_helper.hpp"

#include "core/save_file.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace hui::concepts
{

namespace
{

std::string g_elf = "/app0/assets/helper.elf";
int g_loader_port = 9021; // the PS5's ELF loader
int g_helper_port = 9146; // tools/helper/helper.c HELPER_PORT_DEFAULT

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

// A connection to 127.0.0.1:port, or -1. reply_seconds sets the receive
// timeout (0 leaves it at the default).
int connect_local(int port, int reply_seconds)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
    {
        ::close(fd);
        return -1;
    }
    if (reply_seconds > 0)
    {
        timeval tv{reply_seconds, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

bool send_all(int fd, const char *data, std::size_t len)
{
    while (len > 0)
    {
        const ssize_t n = ::send(fd, data, len, 0);
        if (n <= 0)
            return false;
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

// Sends the helper payload to the ELF loader, which runs it. The loader
// doesn't reply, so this just streams the bytes and closes.
bool send_payload(std::string *why)
{
    std::string elf;
    if (!save::read_file(g_elf, &elf, 16u << 20) || elf.empty())
    {
        *why = "the delete helper is missing from the app";
        return false;
    }
    const int fd = connect_local(g_loader_port, 10);
    if (fd < 0)
    {
        *why = "the PS5's app loader isn't answering (is the jailbreak active?)";
        return false;
    }
    const bool ok = send_all(fd, elf.data(), elf.size());
    ::close(fd);
    if (!ok)
        *why = "couldn't send the delete helper to the app loader";
    return ok;
}

} // namespace

DeleteResult delete_with_helper(const std::string &title_id, const std::string &path,
                                const std::function<void(std::uint64_t, std::uint64_t)> &progress)
{
    return delete_files_with_helper(title_id, path, progress, 0);
}

DeleteResult finish_deletes_with_helper(const std::string &root,
                                        const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                                        int threads)
{
    return delete_files_with_helper("-", root, progress, threads < 1 ? 1 : threads);
}

DeleteResult delete_files_with_helper(const std::string &title_id, const std::string &path,
                                      const std::function<void(std::uint64_t, std::uint64_t)> &progress,
                                      int threads)
{
    DeleteResult result;
    std::string why;
    if (!send_payload(&why))
    {
        result.stage = "loader";
        result.why = why;
        return result;
    }

    // The helper opens its port once the loader has launched it: the loader
    // can take a moment, so keep trying for a good while.
    int fd = -1;
    for (int waited = 0; waited < 20000 && fd < 0; waited += 200)
    {
        fd = connect_local(g_helper_port, 30);
        if (fd < 0)
            sleep_ms(200);
    }
    if (fd < 0)
    {
        result.stage = "helper";
        result.why = "the delete helper didn't start";
        return result;
    }

    // Request: three lines. "-" when there are no files to delete (an
    // installed PKG), which the helper only uninstalls.
    const std::string request =
        title_id == "-" ? "NBD3\n-\n" + path + "\nthreads=" + std::to_string(threads) + "\n"
        : threads > 0   ? "NBD2\n" + title_id + "\n" + path + "\nfiles_only threads=" +
                            std::to_string(threads) + "\n"
                        : "NBD1\n" + title_id + "\n" + (path.empty() ? "-" : path) + "\n";
    if (!send_all(fd, request.data(), request.size()))
    {
        ::close(fd);
        result.stage = "helper";
        result.why = "couldn't reach the delete helper";
        return result;
    }

    // Replies, one per line: "T <total>", "P <done>", "OK", "ERR <stage> <why>".
    std::uint64_t total = 0;
    std::string buffer;
    char chunk[512];
    bool saw_end = false;
    while (!saw_end)
    {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
            break; // the helper closed, or timed out, without "OK"/"ERR"
        buffer.append(chunk, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos)
        {
            const std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (line.rfind("T ", 0) == 0)
            {
                total = std::strtoull(line.c_str() + 2, nullptr, 10);
                if (progress)
                    progress(0, total);
            }
            else if (line.rfind("P ", 0) == 0)
            {
                if (progress)
                    progress(std::strtoull(line.c_str() + 2, nullptr, 10), total);
            }
            else if (line == "OK")
            {
                result.ok = true;
                saw_end = true;
                break;
            }
            else if (line.rfind("ERR ", 0) == 0)
            {
                const std::string rest = line.substr(4);
                const std::size_t space = rest.find(' ');
                result.stage = space == std::string::npos ? rest : rest.substr(0, space);
                result.why = space == std::string::npos ? "" : rest.substr(space + 1);
                saw_end = true;
                break;
            }
        }
    }
    ::close(fd);
    if (!result.ok && result.stage.empty())
    {
        result.stage = "helper";
        result.why = "the delete helper stopped without finishing";
    }
    return result;
}

void set_delete_helper(const std::string &elf_path, int loader_port, int helper_port)
{
    g_elf = elf_path;
    g_loader_port = loader_port;
    g_helper_port = helper_port;
}

} // namespace hui::concepts
