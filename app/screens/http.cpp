// NASBridge - Plain HTTP/WebDAV requests to the NAS, for background threads.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/http.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace hui::concepts
{

namespace
{

constexpr int kConnectSeconds = 5;
constexpr int kReplySeconds = 10;
constexpr int kReceiveBuffer = 4 * 1024 * 1024; // see connect_with_timeout

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// XML text with its entities decoded: "&amp;" "&#91;" "&#x5B;" and friends.
std::string xml_text(const std::string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '&')
        {
            out += text[i];
            continue;
        }
        const std::size_t end = text.find(';', i);
        if (end == std::string::npos || end - i > 10)
        {
            out += text[i];
            continue;
        }
        const std::string name = text.substr(i + 1, end - i - 1);
        long code = -1;
        if (name == "amp")
            code = '&';
        else if (name == "lt")
            code = '<';
        else if (name == "gt")
            code = '>';
        else if (name == "quot")
            code = '"';
        else if (name == "apos")
            code = '\'';
        else if (name.size() > 1 && name[0] == '#')
            code = name[1] == 'x' || name[1] == 'X' ? std::strtol(name.c_str() + 2, nullptr, 16)
                                                    : std::strtol(name.c_str() + 1, nullptr, 10);
        if (code <= 0 || code > 0x10ffff)
        {
            out += text[i];
            continue;
        }
        // As UTF-8.
        const unsigned long c = static_cast<unsigned long>(code);
        if (c < 0x80)
        {
            out += static_cast<char>(c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
        i = end;
    }
    return out;
}

// An href (a path, or a whole URL) as a decoded path without a trailing slash.
std::string href_path(std::string href)
{
    const std::size_t scheme = href.find("://");
    if (scheme != std::string::npos)
    {
        const std::size_t slash = href.find('/', scheme + 3);
        href = slash == std::string::npos ? "/" : href.substr(slash);
    }
    href = percent_decode(href);
    while (href.size() > 1 && href.back() == '/')
        href.pop_back();
    return href;
}

bool send_all(int fd, const std::string &data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
            return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Connects with a time limit. Returns the socket, or -1 with *error set.
int connect_with_timeout(const sockaddr_in &address, int *error,
                         const std::function<bool()> &cancelled = nullptr)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *error = errno;
        return -1;
    }
    // A big receive buffer, asked for before connecting so the window the
    // PS5 advertises can grow: one connection can only have this much on its
    // way, and over Wi-Fi's few milliseconds of latency a small default buffer
    // caps a copy near 6-10 MB/s. The system may give less; that's fine.
    const int buffer = kReceiveBuffer;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
    {
        if (errno != EINPROGRESS)
        {
            *error = errno;
            ::close(fd);
            return -1;
        }
        // Waited for in short slices, so a cancel is noticed quickly.
        int ready = 0;
        for (int waited = 0; waited < kConnectSeconds * 1000 && ready == 0; waited += 250)
        {
            if (cancelled && cancelled())
            {
                *error = ECANCELED;
                ::close(fd);
                return -1;
            }
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            timeval slice{0, 250 * 1000};
            ready = ::select(fd + 1, nullptr, &writable, nullptr, &slice);
            if (ready < 0 && errno == EINTR)
                ready = 0;
        }
        if (ready <= 0)
        {
            *error = ready == 0 ? ETIMEDOUT : errno;
            ::close(fd);
            return -1;
        }
        int result = 0;
        socklen_t length = sizeof(result);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length) != 0 || result != 0)
        {
            *error = result != 0 ? result : errno;
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    timeval reply{kReplySeconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &reply, sizeof(reply));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &reply, sizeof(reply));
    return fd;
}

} // namespace

std::string lowered(const std::string &text)
{
    std::string out = text;
    for (char &c : out)
        c = lower(c);
    return out;
}

bool ends_with_nocase(const std::string &text, const char *suffix)
{
    const std::size_t n = std::strlen(suffix);
    if (text.size() < n)
        return false;
    for (std::size_t i = 0; i < n; ++i)
        if (lower(text[text.size() - n + i]) != lower(suffix[i]))
            return false;
    return true;
}

std::string header_value(const std::string &header, const char *name)
{
    const std::string low = lowered(header);
    const std::string key = std::string("\r\n") + lowered(name) + ":";
    const std::size_t at = low.find(key);
    if (at == std::string::npos)
        return "";
    std::size_t start = at + key.size();
    const std::size_t end = header.find("\r\n", start);
    while (start < header.size() && (header[start] == ' ' || header[start] == '\t'))
        ++start;
    return header.substr(start, (end == std::string::npos ? header.size() : end) - start);
}

std::string percent_decode(const std::string &text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
            hex_value(text[i + 2]) >= 0)
        {
            out += static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            i += 2;
        }
        else
        {
            out += text[i];
        }
    }
    return out;
}

std::string percent_encode_path(const std::string &path)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : path)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                           c == '~' || c == '/';
        if (plain)
        {
            out += ch;
        }
        else
        {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string folder_path(const Connection &connection)
{
    std::string path = connection.folder.empty() ? "/" : connection.folder;
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    if (path.back() != '/')
        path += '/';
    return path;
}

std::string base64(const std::string &text)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i + 2 < text.size())
    {
        const unsigned v = (static_cast<unsigned char>(text[i]) << 16) |
                           (static_cast<unsigned char>(text[i + 1]) << 8) |
                           static_cast<unsigned char>(text[i + 2]);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
        i += 3;
    }
    if (i + 1 == text.size())
    {
        const unsigned v = static_cast<unsigned char>(text[i]) << 16;
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += "==";
    }
    else if (i + 2 == text.size())
    {
        const unsigned v = (static_cast<unsigned char>(text[i]) << 16) |
                           (static_cast<unsigned char>(text[i + 1]) << 8);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool decode_chunked(const std::string &raw, std::string *body)
{
    body->clear();
    std::size_t at = 0;
    for (;;)
    {
        const std::size_t line_end = raw.find("\r\n", at);
        if (line_end == std::string::npos)
            return false;
        std::size_t size = 0;
        int digits = 0;
        for (std::size_t i = at; i < line_end && hex_value(raw[i]) >= 0; ++i, ++digits)
        {
            size = size * 16 + static_cast<std::size_t>(hex_value(raw[i]));
            if (size > raw.size())
                return false;
        }
        if (digits == 0)
            return false;
        at = line_end + 2;
        if (size == 0)
            return true;
        if (at + size > raw.size())
        {
            body->append(raw, at, std::string::npos); // cut short: keep what came
            return true;
        }
        body->append(raw, at, size);
        at += size + 2; // the chunk's "\r\n"
    }
}

const char *propfind_body()
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
           "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/><D:getetag/>"
           "</D:prop></D:propfind>";
}

std::string build_request(const Connection &connection, const std::string &method,
                          const std::string &path, const std::string &extra_headers,
                          const std::string &body)
{
    std::string host = connection.host;
    if (connection.effective_port() != 80)
        host += ":" + std::to_string(connection.effective_port());
    std::string request = method + " " + percent_encode_path(path) + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "User-Agent: NASBridge\r\n";
    request += extra_headers;
    if (!body.empty() || method == "PROPFIND")
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!connection.username.empty())
        request += "Authorization: Basic " +
                   base64(connection.username + ":" + connection.password) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    return request;
}

HttpReply http_request(const Connection &connection, const std::string &method,
                       const std::string &path, const std::string &extra_headers,
                       const std::string &body, std::size_t max_body)
{
    HttpReply reply;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(connection.effective_port()));
    if (::inet_pton(AF_INET, connection.host.c_str(), &address.sin_addr) != 1)
    {
        reply.error = EINVAL;
        return reply;
    }
    const int fd = connect_with_timeout(address, &reply.error);
    if (fd < 0)
        return reply;
    reply.connected = true;

    std::string raw;
    const std::size_t cap = max_body + 64 * 1024; // room for the header
    bool ok = send_all(fd, build_request(connection, method, path, extra_headers, body));
    if (!ok)
        reply.error = errno;
    char buffer[16 * 1024];
    while (ok)
    {
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0)
        {
            raw.append(buffer, static_cast<std::size_t>(n));
            if (raw.size() > cap)
            {
                reply.too_big = true;
                break;
            }
            continue;
        }
        if (n < 0)
            reply.error = errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno;
        break; // 0: the server closed the connection (Connection: close)
    }
    ::close(fd);

    const std::size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos)
    {
        reply.body = std::move(raw); // not HTTP (or nothing): status stays 0
        return reply;
    }
    reply.header = "\r\n" + raw.substr(0, split + 2);
    if (raw.compare(0, 5, "HTTP/") == 0)
    {
        const std::size_t space = raw.find(' ');
        if (space != std::string::npos)
            reply.status = std::atoi(raw.c_str() + space + 1);
    }
    raw.erase(0, split + 4); // the body, without a second copy of it
    reply.body = std::move(raw);
    if (lowered(header_value(reply.header, "Transfer-Encoding")).find("chunked") !=
        std::string::npos)
    {
        std::string decoded;
        if (decode_chunked(reply.body, &decoded))
            reply.body = std::move(decoded);
    }
    else
    {
        const std::string length = header_value(reply.header, "Content-Length");
        if (!length.empty())
        {
            const long n = std::atol(length.c_str());
            if (n >= 0 && static_cast<std::size_t>(n) < reply.body.size())
                reply.body.resize(static_cast<std::size_t>(n));
        }
    }
    if (reply.body.size() > max_body)
    {
        reply.body.resize(max_body);
        reply.too_big = true;
    }
    return reply;
}

namespace
{

// Decodes "Transfer-Encoding: chunked" as it arrives.
class ChunkedStream
{
  public:
    // Feeds raw bytes; decoded ones go to sink. False on a malformed stream
    // or when the sink stops.
    bool feed(const char *data, std::size_t length,
              const std::function<bool(const char *, std::size_t)> &sink)
    {
        std::size_t at = 0;
        while (at < length && !done_)
        {
            switch (state_)
            {
            case State::size:
            {
                const char c = data[at++];
                if (c == '\n')
                {
                    if (digits_ == 0)
                        return false;
                    state_ = remaining_ == 0 ? State::trailer : State::data;
                    if (remaining_ == 0)
                        done_ = true;
                    digits_ = 0;
                }
                else if (!in_extension_)
                {
                    const int v = hex_value(c);
                    if (v >= 0)
                    {
                        remaining_ = remaining_ * 16 + static_cast<std::uint64_t>(v);
                        ++digits_;
                    }
                    else if (c == ';')
                        in_extension_ = true;
                    else if (c != '\r' && c != ' ')
                        return false;
                }
                if (state_ != State::size)
                    in_extension_ = false;
                break;
            }
            case State::data:
            {
                std::size_t take = length - at;
                if (take > remaining_)
                    take = static_cast<std::size_t>(remaining_);
                if (!sink(data + at, take))
                    return false;
                at += take;
                remaining_ -= take;
                if (remaining_ == 0)
                    state_ = State::data_end;
                break;
            }
            case State::data_end:
                if (data[at++] == '\n')
                    state_ = State::size;
                break;
            case State::trailer:
                at = length;
                break;
            }
        }
        return true;
    }
    bool done() const
    {
        return done_;
    }

  private:
    enum class State
    {
        size,
        data,
        data_end,
        trailer,
    };
    State state_ = State::size;
    std::uint64_t remaining_ = 0;
    int digits_ = 0;
    bool in_extension_ = false;
    bool done_ = false;
};

// Hands body bytes to the sink: through the chunk decoder, or up to the
// Content-Length. False when the sink stops or the stream is malformed.
bool deliver(const char *data, std::size_t length, bool chunked, std::uint64_t *left,
             ChunkedStream *decoder, const std::function<bool(const char *, std::size_t)> &sink)
{
    if (chunked)
        return decoder->feed(data, length, sink);
    if (length > *left)
        length = static_cast<std::size_t>(*left);
    if (*left != ~0ull)
        *left -= length;
    return length == 0 || sink(data, length);
}

} // namespace

HttpReply http_stream(const Connection &connection, const std::string &method,
                      const std::string &path, const std::string &extra_headers,
                      const std::string &body,
                      const std::function<bool(const char *data, std::size_t length)> &sink,
                      int reply_seconds, const std::function<bool(const HttpReply &reply)> &on_header,
                      const std::function<bool()> &cancelled)
{
    HttpReply reply;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(connection.effective_port()));
    if (::inet_pton(AF_INET, connection.host.c_str(), &address.sin_addr) != 1)
    {
        reply.error = EINVAL;
        return reply;
    }
    const int fd = connect_with_timeout(address, &reply.error, cancelled);
    if (fd < 0)
        return reply;
    reply.connected = true;
    // With a cancel to watch for, the NAS's silences are waited out a quarter
    // of a second at a time (up to reply_seconds in all).
    timeval quiet{reply_seconds, 0};
    if (cancelled)
        quiet = timeval{0, 250 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &quiet, sizeof(quiet));
    int silent_ms = 0;

    bool ok = send_all(fd, build_request(connection, method, path, extra_headers, body));
    if (!ok)
        reply.error = errno;
    std::string head;          // until the header is complete
    bool in_body = false;
    bool chunked = false;
    std::uint64_t left = ~0ull; // Content-Length, when given
    ChunkedStream decoder;
    bool closed = false; // the NAS ended the connection (the end of an unsized body)
    bool stopped = false;
    // A big read drains the socket in few syscalls and lets TCP keep the window
    // open while a piece is being written to the drive. 512 KB, matching
    // ezremote (libcurl). A plain array (trivial destructor): a thread_local
    // with a non-trivial destructor needs __cxa_thread_atexit_impl, which the
    // PS5 SDK doesn't export. Reused per thread, off the stack (lives in TLS).
    static thread_local char read_buf[512 * 1024];
    char *const buffer = read_buf;
    const std::size_t buffer_size = sizeof(read_buf);
    while (ok)
    {
        const ssize_t n = ::recv(fd, buffer, buffer_size, 0);
        if (n < 0 && cancelled && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (cancelled())
            {
                stopped = true;
                break;
            }
            silent_ms += 250;
            if (silent_ms < reply_seconds * 1000)
                continue; // still within how long the NAS may go quiet
        }
        else if (n > 0)
        {
            silent_ms = 0;
        }
        if (n <= 0)
        {
            if (n < 0)
                reply.error = errno == EAGAIN || errno == EWOULDBLOCK ? ETIMEDOUT : errno;
            else
                closed = true;
            break;
        }
        const char *data = buffer;
        std::size_t length = static_cast<std::size_t>(n);
        if (!in_body)
        {
            head.append(data, length);
            const std::size_t split = head.find("\r\n\r\n");
            if (split == std::string::npos)
            {
                if (head.size() > 64 * 1024)
                    break; // no header end in sight: not HTTP
                continue;
            }
            reply.header = "\r\n" + head.substr(0, split + 2);
            if (head.compare(0, 5, "HTTP/") == 0)
            {
                const std::size_t space = head.find(' ');
                if (space != std::string::npos)
                    reply.status = std::atoi(head.c_str() + space + 1);
            }
            chunked = lowered(header_value(reply.header, "Transfer-Encoding")).find("chunked") !=
                      std::string::npos;
            const std::string content_length = header_value(reply.header, "Content-Length");
            if (!chunked && !content_length.empty())
                left = std::strtoull(content_length.c_str(), nullptr, 10);
            in_body = true;
            if (on_header && !on_header(reply))
            {
                stopped = true;
                break;
            }
            // What came after the header is the start of the body.
            const std::string rest = head.substr(split + 4);
            head.clear();
            if (!rest.empty() && !deliver(rest.data(), rest.size(), chunked, &left, &decoder, sink))
            {
                stopped = true;
                break;
            }
            if (left == 0 || decoder.done())
                break;
            continue;
        }
        if (!deliver(data, length, chunked, &left, &decoder, sink))
        {
            stopped = true;
            break;
        }
        if (left == 0 || decoder.done())
            break;
    }
    ::close(fd);
    reply.complete = in_body && !stopped &&
                     (chunked ? decoder.done() : left == ~0ull ? closed : left == 0);
    return reply;
}

bool http_read_range(const Connection &connection, const std::string &path,
                     std::uint64_t offset, std::size_t length, std::string *out, std::string *why)
{
    out->clear();
    if (length == 0)
        return true;
    const std::string range = "Range: bytes=" + std::to_string(offset) + "-" +
                              std::to_string(offset + length - 1) + "\r\n";
    HttpReply reply = http_request(connection, "GET", path, range, "", length);
    if (reply.status == 206 || (reply.status == 200 && offset == 0))
    {
        // A 200 means the NAS ignored the range and sent the file from the
        // start: fine for the first bytes (the rest was cut off at `length`).
        if (reply.body.size() >= length)
        {
            reply.body.resize(length);
            *out = std::move(reply.body);
            return true;
        }
        *why = "the NAS sent less of the file than asked for";
        return false;
    }
    if (reply.status == 200)
        *why = "the NAS can't send part of a file (no Range support)";
    else if (!reply.connected)
        *why = "couldn't connect (error " + std::to_string(reply.error) + ")";
    else
        *why = "the NAS answered HTTP " + std::to_string(reply.status);
    return false;
}

bool parse_listing(const std::string &body, const std::string &folder, std::vector<DavEntry> *entries)
{
    entries->clear();
    std::string self = percent_decode(folder);
    while (self.size() > 1 && self.back() == '/')
        self.pop_back();
    int responses = 0;
    bool in_response = false;
    bool collection = false;
    std::string href;
    std::string length;
    std::string modified;
    std::string etag;
    std::size_t at = 0;
    while ((at = body.find('<', at)) != std::string::npos)
    {
        const bool closing = at + 1 < body.size() && body[at + 1] == '/';
        std::size_t end = at + (closing ? 2 : 1);
        while (end < body.size() && body[end] != '>' && body[end] != ' ' && body[end] != '/' &&
               body[end] != '\t' && body[end] != '\r' && body[end] != '\n')
            ++end;
        std::string name = body.substr(at + (closing ? 2 : 1), end - at - (closing ? 2 : 1));
        const std::size_t colon = name.find(':');
        if (colon != std::string::npos)
            name = name.substr(colon + 1);
        name = lowered(name);

        // The text after this tag, up to the next one.
        const auto text_after = [&]() -> std::string
        {
            const std::size_t open_end = body.find('>', at);
            if (open_end == std::string::npos)
                return "";
            const std::size_t text_end = body.find('<', open_end + 1);
            if (text_end == std::string::npos)
                return "";
            return body.substr(open_end + 1, text_end - open_end - 1);
        };

        if (name == "response")
        {
            if (!closing)
            {
                in_response = true;
                collection = false;
                href.clear();
                length.clear();
                modified.clear();
                etag.clear();
            }
            else if (in_response)
            {
                in_response = false;
                ++responses;
                const std::string path = href_path(href);
                const std::size_t slash = path.rfind('/');
                const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
                const bool skip = path == self || leaf.empty() || leaf[0] == '.' ||
                                  leaf[0] == '@' || leaf[0] == '#';
                if (!skip)
                {
                    DavEntry entry;
                    entry.name = leaf;
                    entry.path = path;
                    entry.folder = collection;
                    entry.size = std::strtoull(length.c_str(), nullptr, 10);
                    entry.modified = modified;
                    entry.etag = etag;
                    entries->push_back(std::move(entry));
                }
            }
        }
        else if (!closing && in_response && name == "href")
        {
            href = xml_text(text_after());
        }
        else if (!closing && in_response && name == "getcontentlength")
        {
            length = xml_text(text_after());
        }
        else if (!closing && in_response && name == "getlastmodified")
        {
            modified = xml_text(text_after());
        }
        else if (!closing && in_response && name == "getetag")
        {
            etag = xml_text(text_after());
        }
        else if (!closing && in_response && name == "collection")
        {
            collection = true;
        }
        at = end;
    }
    return responses > 0;
}

} // namespace hui::concepts
