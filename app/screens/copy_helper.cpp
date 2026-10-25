// NASBridge - Copying a file through the copy helper payload.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/copy_helper.hpp"

#include "core/save_file.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace hui::concepts
{

namespace
{

std::string g_elf = "/app0/assets/copier.elf";
int g_loader_port = 9021; // the PS5's ELF loader
int g_helper_port = 9147; // tools/copier/copier.c COPIER_PORT_DEFAULT
std::mutex g_launch;      // one launch at a time
// Two connections: on PS5 Wi-Fi one tops out near 40 MB/s while the NAS can
// send more; two neighbouring chunks at a time keep its disk reading nearly
// in order (three or four far-apart readers made a single disk seek itself
// slow).
int g_helper_streams = 2;

constexpr const char *kProto = "NBC1";
constexpr const char *kBatchProto = "NBB1";
constexpr const char *kSmbProto = "NBS1";      // one file over SMB
constexpr const char *kSmbBatchProto = "NBT1"; // small files over SMB
constexpr const char *kFtpProto = "NBG1";      // one file over FTP
constexpr const char *kFtpBatchProto = "NBH1"; // small files over FTP
constexpr const char *kPartitionProto = "NBP1"; // a partition's type
constexpr const char *kFileStepProto = "NBF1";  // a step on a drive's files
constexpr int kStartMs = 20000;   // the loader can take a while to run it
constexpr int kSilentMs = 90000;  // longer than the helper ever goes quiet

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

// A connection to 127.0.0.1:port, or -1.
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
    timeval tv{reply_seconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

bool send_all(int fd, const char *data, std::size_t len)
{
    while (len > 0)
    {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data, len, 0);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

// Sends the helper to the ELF loader, which runs it.
bool send_payload()
{
    std::string elf;
    if (!save::read_file(g_elf, &elf, 16u << 20) || elf.empty())
        return false;
    const int fd = connect_local(g_loader_port, 10);
    if (fd < 0)
        return false;
    const bool ok = send_all(fd, elf.data(), elf.size());
    ::close(fd);
    return ok;
}

// One line from the helper, waiting up to `ms`; false when it closed or said
// nothing in time.
bool read_line(int fd, std::string *buffer, std::string *line, int ms)
{
    for (int waited = 0;; waited += 250)
    {
        const std::size_t nl = buffer->find('\n');
        if (nl != std::string::npos)
        {
            *line = buffer->substr(0, nl);
            buffer->erase(0, nl + 1);
            return true;
        }
        if (waited >= ms)
            return false;
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 250);
        if (r < 0 && errno != EINTR)
            return false;
        if (r <= 0)
            continue;
        char chunk[512];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
            return false;
        buffer->append(chunk, static_cast<std::size_t>(n));
    }
}

// A connection to a running helper that has said it's ready, starting one
// through the loader when none answers. -1 when it can't be had.
int open_helper(std::string *buffer)
{
    std::lock_guard<std::mutex> hold(g_launch);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        int fd = connect_local(g_helper_port, 10);
        if (fd < 0)
        {
            if (!send_payload())
                return -1;
            for (int waited = 0; waited < kStartMs && fd < 0; waited += 200)
            {
                sleep_ms(200);
                fd = connect_local(g_helper_port, 10);
            }
            if (fd < 0)
                return -1;
        }
        buffer->clear();
        std::string line;
        if (read_line(fd, buffer, &line, 10000) && line == kProto)
            return fd;
        // A helper that was just leaving (idle), or another version: once more.
        ::close(fd);
        sleep_ms(300);
    }
    return -1;
}

// The sign-in as the helper takes it: its fields, each ended by a NUL.
std::string sign_in_bytes(const SmbSignIn &sign_in, const std::string *path)
{
    std::string bytes = sign_in.share + '\0';
    if (path != nullptr)
        bytes += *path + '\0';
    bytes += sign_in.user + '\0' + sign_in.domain + '\0' + sign_in.password + '\0';
    return bytes;
}

// FTP's: the path (for one file), the user and the password, each ended by a NUL.
std::string sign_in_bytes(const FtpSignIn &sign_in, const std::string *path)
{
    std::string bytes;
    if (path != nullptr)
        bytes += *path + '\0';
    bytes += sign_in.user + '\0' + sign_in.password + '\0';
    return bytes;
}

using Progress = std::function<void(std::uint64_t, std::uint64_t, std::uint64_t, int)>;

// One file: sends the job line and what follows it, then follows the helper
// to the end.
HelperCopy run_copy(const std::string &job, const std::string &payload, const Progress &progress,
                    const std::function<bool()> &stopping)
{
    HelperCopy result;
    std::string buffer;
    const int fd = open_helper(&buffer);
    if (fd < 0)
    {
        result.end = HelperCopy::End::unavailable;
        result.why = "the copy helper didn't start";
        return result;
    }
    if (!send_all(fd, job.data(), job.size()) || !send_all(fd, payload.data(), payload.size()))
    {
        ::close(fd);
        result.end = HelperCopy::End::lost;
        result.why = "couldn't reach the copy helper";
        return result;
    }

    int silent = 0;
    while (true)
    {
        if (stopping && stopping())
        {
            send_all(fd, "STOP\n", 5); // it stops at once; nothing to wait for
            ::close(fd);
            result.end = HelperCopy::End::stopped;
            return result;
        }
        std::string line;
        if (!read_line(fd, &buffer, &line, 250))
        {
            // Nothing for a quarter second: still there?
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 0) > 0 && buffer.find('\n') == std::string::npos)
            {
                char chunk[512];
                const ssize_t n = ::recv(fd, chunk, sizeof(chunk), MSG_PEEK);
                if (n <= 0)
                    break; // closed without a last word
            }
            silent += 250;
            if (silent >= kSilentMs)
                break;
            continue;
        }
        silent = 0;
        if (line.rfind("S ", 0) == 0)
        {
            result.status = std::atoi(line.c_str() + 2);
        }
        else if (line.rfind("P ", 0) == 0)
        {
            char *end = nullptr;
            const std::uint64_t at = std::strtoull(line.c_str() + 2, &end, 10);
            const std::uint64_t write_ms = std::strtoull(end, &end, 10);
            const std::uint64_t elapsed_ms = std::strtoull(end, &end, 10);
            const int connections = static_cast<int>(std::strtol(end, &end, 10));
            result.position = at;
            if (progress)
                progress(at, write_ms, elapsed_ms, connections);
        }
        else if (line == "OK" || line == "STOPPED")
        {
            ::close(fd);
            result.end = line == "OK" ? HelperCopy::End::ok : HelperCopy::End::stopped;
            return result;
        }
        else if (line.rfind("ERR ", 0) == 0)
        {
            const std::string rest = line.substr(4);
            const std::size_t space = rest.find(' ');
            const std::string kind = rest.substr(0, space);
            result.why = space == std::string::npos ? "" : rest.substr(space + 1);
            using End = HelperCopy::End;
            if (kind == "file")
                result.end = End::file;
            else if (kind == "write")
                result.end = End::write;
            else if (kind == "connect" || kind == "drop" || kind == "timeout")
                result.end = End::network;
            else if (kind == "http")
            {
                result.end = End::refused;
                result.status = std::atoi(result.why.c_str());
            }
            else if (kind == "smb" || kind == "ftp")
                result.end = End::refused; // the NAS said no: `why` says what
            else if (kind == "chunked")
                result.end = End::unsupported;
            else if (kind == "changed")
                result.end = End::changed;
            else
                result.end = End::unavailable; // "request": it didn't understand us
            ::close(fd);
            return result;
        }
    }
    ::close(fd);
    result.end = HelperCopy::End::lost;
    result.why = "the copy helper stopped answering";
    return result;
}

// Many files: sends `opening` (the batch line and what follows it), then the
// list as the helper takes it, following the helper to the end.
HelperBatch run_batch(const std::string &opening, const std::vector<BatchFile> &files,
                      const std::function<void(std::size_t)> &done,
                      const std::function<void()> &synced, const BatchProgress &progress,
                      const std::function<bool()> &stopping)
{
    using End = HelperCopy::End;
    HelperBatch result;
    std::string in;
    const int fd = open_helper(&in);
    if (fd < 0)
    {
        result.end = End::unavailable;
        result.why = "the copy helper didn't start";
        return result;
    }
    // Sending and reading at once: the helper answers while it still takes
    // the list, and neither side may wait on the other.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    std::string out = opening;
    std::size_t sent = 0;     // of `out`
    std::size_t next = 0;     // the next file to put in `out`
    bool ended = false;       // "E" is in `out`
    bool stop_sent = false;
    int silent = 0;
    const auto finish = [&](End end)
    {
        ::close(fd);
        result.end = end;
        return result;
    };
    while (true)
    {
        if (!stop_sent && stopping && stopping())
        {
            // Whatever is half sent is dropped: the helper stops at once.
            ::fcntl(fd, F_SETFL, flags);
            send_all(fd, "\nSTOP\n", 6);
            return finish(End::stopped);
        }
        // Keep a little of the list on its way.
        while (out.size() - sent < 64 * 1024 && !ended)
        {
            if (next == files.size())
            {
                out += "E\n";
                ended = true;
                break;
            }
            const BatchFile &file = files[next];
            out += "F\t" + std::to_string(next) + "\t" + std::to_string(file.size) + "\t" +
                   file.local + "\t" + std::to_string(file.request.size()) + "\n" + file.request;
            ++next;
        }
        if (sent > 256 * 1024)
        {
            out.erase(0, sent);
            sent = 0;
        }
        pollfd p{fd, static_cast<short>(POLLIN | (sent < out.size() ? POLLOUT : 0)), 0};
        const int r = ::poll(&p, 1, 250);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0 && (p.revents & POLLOUT) && sent < out.size())
        {
#ifdef MSG_NOSIGNAL
            const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
#endif
            if (n > 0)
                sent += static_cast<std::size_t>(n);
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                break;
        }
        bool heard = false;
        if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            char chunk[4096];
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n == 0)
                break; // closed without a last word
            if (n > 0)
            {
                in.append(chunk, static_cast<std::size_t>(n));
                heard = true;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                break;
        }
        std::size_t nl;
        while ((nl = in.find('\n')) != std::string::npos)
        {
            const std::string line = in.substr(0, nl);
            in.erase(0, nl + 1);
            if (line.rfind("D ", 0) == 0)
            {
                const unsigned long index = std::strtoul(line.c_str() + 2, nullptr, 10);
                if (done && index < files.size())
                    done(index);
            }
            else if (line == "Y")
            {
                if (synced)
                    synced();
            }
            else if (line.rfind("P ", 0) == 0)
            {
                char *end = nullptr;
                const std::uint64_t bytes = std::strtoull(line.c_str() + 2, &end, 10);
                const std::uint64_t count = std::strtoull(end, &end, 10);
                const int busy = static_cast<int>(std::strtol(end, &end, 10));
                // Newer helpers add the time spent saving and in all.
                const std::uint64_t saving_ms = std::strtoull(end, &end, 10);
                const std::uint64_t elapsed_ms = std::strtoull(end, &end, 10);
                if (progress)
                    progress(bytes, count, busy, saving_ms, elapsed_ms);
            }
            else if (line == "OK" || line == "STOPPED")
                return finish(line == "OK" ? End::ok : End::stopped);
            else if (line.rfind("ERR ", 0) == 0)
            {
                // "ERR <index> <kind> <why>"; an older helper: "ERR request ...".
                std::string rest = line.substr(4);
                char *end = nullptr;
                const long index = std::strtol(rest.c_str(), &end, 10);
                if (end != rest.c_str())
                {
                    result.failed = index;
                    rest = end;
                    while (!rest.empty() && rest[0] == ' ')
                        rest.erase(0, 1);
                }
                const std::size_t space = rest.find(' ');
                const std::string kind = rest.substr(0, space);
                result.why = space == std::string::npos ? "" : rest.substr(space + 1);
                if (kind == "file")
                    return finish(End::file);
                if (kind == "write")
                    return finish(End::write);
                if (kind == "connect" || kind == "drop" || kind == "timeout")
                    return finish(End::network);
                if (kind == "http")
                {
                    result.status = std::atoi(result.why.c_str());
                    return finish(End::refused);
                }
                if (kind == "smb" || kind == "ftp")
                    return finish(End::refused);
                if (kind == "changed")
                    return finish(End::changed);
                if (kind == "chunked")
                    return finish(End::unsupported);
                // "request": an older helper that copies one file at a time.
                return finish(result.failed < 0 ? End::unsupported : End::unavailable);
            }
        }
        if (heard)
            silent = 0;
        else if ((silent += 250) >= kSilentMs)
            break;
    }
    result.why = "the copy helper stopped answering";
    return finish(End::lost);
}

} // namespace

HelperCopy copy_with_helper(const std::string &ip, int port, std::uint64_t size,
                            const std::string &local, const std::string &request, int streams,
                            const Progress &progress, const std::function<bool()> &stopping)
{
    const std::string job = std::string(kProto) + "\t" + ip + "\t" + std::to_string(port) + "\t" +
                            std::to_string(size) + "\t" + local + "\t" +
                            std::to_string(request.size()) + "\t" + std::to_string(streams) + "\n";
    return run_copy(job, request, progress, stopping);
}

HelperCopy copy_smb_with_helper(const std::string &ip, int port, std::uint64_t size,
                                const std::string &local, const SmbSignIn &sign_in,
                                const std::string &path, int streams, const Progress &progress,
                                const std::function<bool()> &stopping)
{
    const std::string bytes = sign_in_bytes(sign_in, &path);
    const std::string job = std::string(kSmbProto) + "\t" + ip + "\t" + std::to_string(port) +
                            "\t" + std::to_string(size) + "\t" + local + "\t" +
                            std::to_string(bytes.size()) + "\t" + std::to_string(streams) + "\n";
    return run_copy(job, bytes, progress, stopping);
}

HelperBatch copy_batch_with_helper(const std::string &ip, int port, int workers,
                                   const std::vector<BatchFile> &files,
                                   const std::function<void(std::size_t)> &done,
                                   const std::function<void()> &synced,
                                   const BatchProgress &progress,
                                   const std::function<bool()> &stopping)
{
    return run_batch(std::string(kBatchProto) + "\t" + ip + "\t" + std::to_string(port) + "\t" +
                         std::to_string(workers) + "\n",
                     files, done, synced, progress, stopping);
}

HelperBatch copy_smb_batch_with_helper(const std::string &ip, int port, int workers,
                                       const SmbSignIn &sign_in, const std::vector<BatchFile> &files,
                                       const std::function<void(std::size_t)> &done,
                                       const std::function<void()> &synced,
                                       const BatchProgress &progress,
                                       const std::function<bool()> &stopping)
{
    const std::string bytes = sign_in_bytes(sign_in, nullptr);
    return run_batch(std::string(kSmbBatchProto) + "\t" + ip + "\t" + std::to_string(port) +
                         "\t" + std::to_string(workers) + "\t" + std::to_string(bytes.size()) +
                         "\n" + bytes,
                     files, done, synced, progress, stopping);
}

HelperCopy copy_ftp_with_helper(const std::string &ip, int port, std::uint64_t size,
                                const std::string &local, const FtpSignIn &sign_in,
                                const std::string &path, int streams, const Progress &progress,
                                const std::function<bool()> &stopping)
{
    const std::string bytes = sign_in_bytes(sign_in, &path);
    const std::string job = std::string(kFtpProto) + "\t" + ip + "\t" + std::to_string(port) +
                            "\t" + std::to_string(size) + "\t" + local + "\t" +
                            std::to_string(bytes.size()) + "\t" + std::to_string(streams) + "\n";
    return run_copy(job, bytes, progress, stopping);
}

HelperBatch copy_ftp_batch_with_helper(const std::string &ip, int port, int workers,
                                       const FtpSignIn &sign_in, const std::vector<BatchFile> &files,
                                       const std::function<void(std::size_t)> &done,
                                       const std::function<void()> &synced,
                                       const BatchProgress &progress,
                                       const std::function<bool()> &stopping)
{
    const std::string bytes = sign_in_bytes(sign_in, nullptr);
    return run_batch(std::string(kFtpBatchProto) + "\t" + ip + "\t" + std::to_string(port) +
                         "\t" + std::to_string(workers) + "\t" + std::to_string(bytes.size()) +
                         "\n" + bytes,
                     files, done, synced, progress, stopping);
}

bool copy_helper_enabled()
{
    return !g_elf.empty();
}

std::string partition_type(const std::string &device, std::string *why)
{
    if (!copy_helper_enabled())
    {
        *why = "no copy helper";
        return "";
    }
    std::string buffer;
    const int fd = open_helper(&buffer);
    if (fd < 0)
    {
        *why = "the copy helper didn't start";
        return "";
    }
    const std::string job = std::string(kPartitionProto) + "\t" + device + "\n";
    std::string line;
    const bool answered = send_all(fd, job.data(), job.size()) && read_line(fd, &buffer, &line, 10000);
    ::close(fd);
    if (answered && line.compare(0, 2, "T ") == 0)
        return line.substr(2);
    *why = !answered ? "the copy helper didn't answer"
           : line.compare(0, 4, "ERR ") == 0 ? line.substr(4)
                                             : "an older copy helper";
    return "";
}

int helper_file_step(const char *step, const std::string &path, const std::string &to,
                     std::uint64_t *size, bool *folder)
{
    if (!copy_helper_enabled())
        return ENOSYS;
    std::string buffer;
    const int fd = open_helper(&buffer);
    if (fd < 0)
        return EIO;
    std::string job = std::string(kFileStepProto) + "\t" + step + "\t" + path;
    if (!to.empty())
        job += "\t" + to;
    job += "\n";
    std::string line;
    const bool answered = send_all(fd, job.data(), job.size()) && read_line(fd, &buffer, &line, 30000);
    ::close(fd);
    if (!answered)
        return EIO;
    if (line.compare(0, 2, "OK") == 0)
    {
        unsigned long long bytes = 0;
        char kind = 'f';
        if (std::sscanf(line.c_str(), "OK %llu %c", &bytes, &kind) == 2)
        {
            if (size != nullptr)
                *size = bytes;
            if (folder != nullptr)
                *folder = kind == 'd';
        }
        return 0;
    }
    int error = 0;
    if (std::sscanf(line.c_str(), "ERR %d", &error) == 1 && error > 0)
        return error;
    return ENOSYS; // "ERR request ...": an older helper
}

void set_copy_helper(const std::string &elf_path, int loader_port, int helper_port)
{
    g_elf = elf_path;
    g_loader_port = loader_port;
    g_helper_port = helper_port;
}

int g_batch_workers = 16; // measured: a one-disk NAS served 105 files/s alone, 303 at 4, 538 at 16

int batch_workers()
{
    return g_batch_workers;
}

void set_batch_workers(int workers)
{
    g_batch_workers = workers < 1 ? 1 : workers > 16 ? 16 : workers;
}

int helper_streams()
{
    return g_helper_streams;
}

void set_helper_streams(int streams)
{
    g_helper_streams = streams < 1 ? 1 : streams > 4 ? 4 : streams;
}

} // namespace hui::concepts
