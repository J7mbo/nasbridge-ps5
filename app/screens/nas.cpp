// NASBridge - Reading the NAS over WebDAV, SMB or FTP, whichever the connection uses.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/nas.hpp"

#include <cerrno>

namespace hui::concepts
{

namespace
{

const char *const kDepthOne = "Depth: 1\r\nContent-Type: application/xml; charset=utf-8\r\n";

// Walking a game folder over SMB: sign-ins, and listings in flight on each.
// A NAS answers each sign-in's requests one after another, so a NAS slow to
// read its disks (Unraid's shared folders) is only faster with more sign-ins;
// a few listings in flight on each hide the network's delay as well.
constexpr int kSmbWalkSignIns = 4;
constexpr int kSmbWalkAtOnce = 4;
// Over FTP, sign-ins only: each lists one folder at a time. As many as the
// SMB walk has listings in flight, less what a NAS's per-address limit turns
// away (those just aren't used).
constexpr int kFtpWalkConnections = 8;

// A failed reply in a few words: "not found (HTTP 404)".
std::string describe(const HttpReply &reply)
{
    if (!reply.connected)
        return "couldn't connect (error " + std::to_string(reply.error) + ")";
    switch (reply.status)
    {
    case 0:
        return reply.error != 0 ? "no answer (error " + std::to_string(reply.error) + ")"
                                : "no answer";
    case 401:
        return "sign-in refused (HTTP 401)";
    case 403:
        return "access denied (HTTP 403)";
    case 404:
        return "not found (HTTP 404)";
    default:
        return "HTTP " + std::to_string(reply.status);
    }
}

// Whether the NAS answered at all.
bool answered(const SmbFailure &failure)
{
    using Kind = SmbFailure::Kind;
    return failure.kind != Kind::address && failure.kind != Kind::connect &&
           failure.kind != Kind::refused && failure.kind != Kind::timeout;
}

bool answered(const FtpFailure &failure)
{
    using Kind = FtpFailure::Kind;
    return failure.kind != Kind::address && failure.kind != Kind::connect &&
           failure.kind != Kind::refused && failure.kind != Kind::timeout &&
           failure.kind != Kind::lost && failure.kind != Kind::passive;
}

template <typename Entry>
std::vector<DavEntry> as_listing(const std::string &folder, const std::vector<Entry> &entries,
                                 const std::function<std::string(const Entry &)> &modified)
{
    std::string base = folder;
    if (base.empty() || base.back() != '/')
        base += '/';
    std::vector<DavEntry> listing;
    listing.reserve(entries.size());
    for (const Entry &entry : entries)
    {
        DavEntry item;
        item.name = entry.name;
        item.path = base + entry.name;
        item.folder = entry.folder;
        item.size = entry.size;
        item.modified = modified(entry);
        listing.push_back(std::move(item));
    }
    return listing;
}

} // namespace

std::vector<DavEntry> smb_as_listing(const std::string &folder, const std::vector<SmbEntry> &entries)
{
    return as_listing<SmbEntry>(folder, entries,
                                [](const SmbEntry &entry) { return std::to_string(entry.modified); });
}

std::vector<DavEntry> ftp_as_listing(const std::string &folder, const std::vector<FtpEntry> &entries)
{
    return as_listing<FtpEntry>(folder, entries, [](const FtpEntry &entry) { return entry.modified; });
}

NasReader::NasReader(const Connection &connection, int seconds)
    : connection_(connection), seconds_(seconds)
{
}

NasReader::~NasReader() = default;

SmbSession &NasReader::session()
{
    if (!session_)
        session_ = std::make_unique<SmbSession>(connection_, seconds_);
    return *session_;
}

FtpSession &NasReader::ftp_session()
{
    if (!ftp_)
        ftp_ = std::make_unique<FtpSession>(connection_, seconds_);
    return *ftp_;
}

bool NasReader::list(const std::string &folder, std::vector<DavEntry> *entries, NasProblem *problem)
{
    entries->clear();
    NasProblem scratch;
    NasProblem &why = problem != nullptr ? *problem : scratch;
    why = NasProblem{};
    if (smb())
    {
        std::vector<SmbEntry> inside;
        if (!session().list(folder, &inside, &why.smb))
        {
            why.brief = smb_failure_brief(why.smb);
            why.reached = answered(why.smb);
            return false;
        }
        *entries = smb_as_listing(folder, inside);
        return true;
    }
    if (ftp())
    {
        std::vector<FtpEntry> inside;
        if (!ftp_session().list(folder, &inside, &why.ftp))
        {
            why.brief = ftp_failure_brief(why.ftp);
            why.reached = answered(why.ftp);
            return false;
        }
        *entries = ftp_as_listing(folder, inside);
        return true;
    }
    why.reply = http_request(connection_, "PROPFIND", folder, kDepthOne, propfind_body());
    if (why.reply.status == 207 && parse_listing(why.reply.body, folder, entries))
        return true;
    why.brief = describe(why.reply);
    why.reached = why.reply.connected;
    return false;
}

bool NasReader::walk(
    const std::string &folder,
    const std::function<bool(const std::string &folder, const std::vector<DavEntry> &entries)> &found,
    NasProblem *problem)
{
    NasProblem scratch;
    NasProblem &why = problem != nullptr ? *problem : scratch;
    why = NasProblem{};
    const std::string top = folder.empty() || folder.back() != '/' ? folder + "/" : folder;
    if (ftp())
    {
        const bool ok = ftp_session().walk(
            folder, kFtpWalkConnections,
            [&](const std::string &inside, const std::vector<FtpEntry> &entries)
            {
                const std::string path = top + inside;
                return found(path, ftp_as_listing(path, entries));
            },
            &why.ftp);
        if (!ok && why.ftp.kind != FtpFailure::Kind::none)
        {
            why.brief = ftp_failure_brief(why.ftp);
            why.reached = answered(why.ftp);
        }
        return ok;
    }
    const bool ok = session().walk(
        folder, kSmbWalkSignIns, kSmbWalkAtOnce,
        [&](const std::string &inside, const std::vector<SmbEntry> &entries)
        {
            const std::string path = top + inside;
            return found(path, smb_as_listing(path, entries));
        },
        &why.smb);
    if (!ok && why.smb.kind != SmbFailure::Kind::none)
    {
        why.brief = smb_failure_brief(why.smb);
        why.reached = answered(why.smb);
    }
    return ok;
}

bool NasReader::read_file(const std::string &path, std::size_t max, std::string *out,
                          NasProblem *problem)
{
    out->clear();
    NasProblem scratch;
    NasProblem &why = problem != nullptr ? *problem : scratch;
    why = NasProblem{};
    if (smb())
    {
        bool too_big = false;
        if (session().read_file(path, max, out, &too_big, &why.smb))
            return true;
        why.brief = too_big ? "too big" : smb_failure_brief(why.smb);
        why.reached = too_big || answered(why.smb);
        return false;
    }
    if (ftp())
    {
        bool too_big = false;
        if (ftp_session().read_file(path, max, out, &too_big, &why.ftp))
            return true;
        why.brief = too_big ? "too big" : ftp_failure_brief(why.ftp);
        why.reached = too_big || answered(why.ftp);
        return false;
    }
    why.reply = http_request(connection_, "GET", path, "", "", max);
    if (why.reply.status == 200 && !why.reply.too_big)
    {
        *out = std::move(why.reply.body);
        return true;
    }
    why.brief = why.reply.too_big ? "too big" : describe(why.reply);
    why.reached = why.reply.connected;
    return false;
}

bool NasReader::read_range(const std::string &path, std::uint64_t offset, std::size_t length,
                           std::string *out, std::string *why)
{
    if (ftp())
    {
        FtpFailure failure;
        if (ftp_session().read_range(path, offset, length, out, &failure))
            return true;
        *why = ftp_failure_brief(failure);
        return false;
    }
    if (!smb())
        return http_read_range(connection_, path, offset, length, out, why);
    SmbFailure failure;
    if (session().read_range(path, offset, length, out, &failure))
        return true;
    *why = smb_failure_brief(failure);
    return false;
}

ProbeResult explain_nas_failure(const Connection &connection, const NasProblem &problem)
{
    if (connection.type == ConnectionType::smb)
    {
        ProbeResult result;
        result.state = ProbeResult::State::failed;
        describe_smb_failure(connection, problem.smb, &result.headline, &result.detail);
        return result;
    }
    if (connection.type == ConnectionType::ftp)
    {
        ProbeResult result;
        result.state = ProbeResult::State::failed;
        describe_ftp_failure(connection, problem.ftp, &result.headline, &result.detail);
        return result;
    }
    const HttpReply &reply = problem.reply;
    if (!reply.connected)
        return interpret_socket_error(connection, reply.error, true);
    if (reply.header.empty() && reply.body.empty())
        return interpret_socket_error(connection, reply.error != 0 ? reply.error : ECONNRESET,
                                      false);
    return interpret_response(connection, reply.status, reply.header, reply.body);
}

} // namespace hui::concepts
