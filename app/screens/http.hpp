// NASBridge - Plain HTTP/WebDAV requests to the NAS, for background threads.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One request per connection ("Connection: close"), BSD sockets from
// libkernel, Basic sign-in, chunked replies understood. Every call blocks, so
// call them from a worker thread only. IPv4 addresses only: names can't be
// looked up from a native PS5 app yet.

#pragma once

#include "concepts/connection.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hui::concepts
{

struct HttpReply
{
    bool connected = false; // false: `error` says why the connection failed
    int error = 0;          // errno from connecting or reading (0: none)
    int status = 0;         // HTTP status; 0 when the reply wasn't HTTP
    std::string header;     // starts with "\r\n", so header_value can search it
    std::string body;       // decoded (chunked or Content-Length)
    bool too_big = false;   // the body went over max_body and was cut
    bool complete = false;  // http_stream: the whole body arrived
};

// Sends one request for `path` (decoded, starting with '/') and reads the
// reply. extra_headers: complete "Name: value\r\n" lines.
HttpReply http_request(const Connection &connection, const std::string &method,
                       const std::string &path, const std::string &extra_headers = "",
                       const std::string &body = "", std::size_t max_body = 8u * 1024u * 1024u);

// Like http_request, but the body isn't kept: it is handed to `sink` piece by
// piece as it arrives (chunked replies already decoded), so a reply of any
// size needs only a little memory. `sink` returns false to stop early.
// reply_seconds: how long the NAS may go quiet (a big listing takes a while
// to start). The returned reply has an empty body.
// on_header, when given, sees the status and header before any body is
// delivered; returning false ends the request there. cancelled, when given,
// is asked a few times a second while connecting and while the NAS is quiet:
// true ends the request at once (a pause shouldn't wait for the network).
HttpReply http_stream(const Connection &connection, const std::string &method,
                      const std::string &path, const std::string &extra_headers,
                      const std::string &body,
                      const std::function<bool(const char *data, std::size_t length)> &sink,
                      int reply_seconds = 60,
                      const std::function<bool(const HttpReply &reply)> &on_header = nullptr,
                      const std::function<bool()> &cancelled = nullptr);

// Reads `length` bytes at `offset` of a file with a Range request. False (and
// *why) when the NAS can't, or doesn't send exactly those bytes.
bool http_read_range(const Connection &connection, const std::string &path,
                     std::uint64_t offset, std::size_t length, std::string *out, std::string *why);

// The request http_request sends (for tests).
std::string build_request(const Connection &connection, const std::string &method,
                          const std::string &path, const std::string &extra_headers,
                          const std::string &body);

// One entry of a WebDAV folder listing.
struct DavEntry
{
    std::string name; // decoded, without slashes
    std::string path; // decoded, from the server's root ("/games/Game/sce_sys/param.json")
    bool folder = false;
    std::uint64_t size = 0; // files only (getcontentlength)
    std::string modified;   // getlastmodified, as the NAS writes it
    std::string etag;       // getetag ("" when the NAS gives none)
};

// Reads a PROPFIND (Depth: 1) reply. The folder itself, and hidden or
// NAS-internal entries (".x", "@eaDir", "#recycle"), are left out. False when
// the body holds no <response> at all.
bool parse_listing(const std::string &body, const std::string &folder, std::vector<DavEntry> *entries);

// The PROPFIND request body asking for what parse_listing reads.
const char *propfind_body();

// ---- helpers ----
std::string header_value(const std::string &header, const char *name);
std::string percent_decode(const std::string &text);
std::string percent_encode_path(const std::string &path);
// "/games/" for any folder setting.
std::string folder_path(const Connection &connection);
std::string lowered(const std::string &text);
bool ends_with_nocase(const std::string &text, const char *suffix);
bool decode_chunked(const std::string &raw, std::string *body);
std::string base64(const std::string &text);

} // namespace hui::concepts
