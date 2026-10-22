// NASBridge - The SMB layer (smb.cpp) and the connection's SMB settings,
// without a NAS: folder paths, user names, what is left out of listings, the
// failures it explains, and a sign-in to a port nothing listens on. Run on a
// computer.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/connection.hpp"
#include "concepts/smb.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace hui::concepts;

namespace
{

int g_failures = 0;

void check(bool ok, const std::string &what, const std::string &detail = "")
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  [",
                detail.empty() ? "" : (detail + "]").c_str());
    if (!ok)
        ++g_failures;
}

// A local port nothing listens on (bound once, then let go).
int free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
    ::close(fd);
    return ntohs(address.sin_port);
}

bool located(const std::string &folder, const std::string &share, const std::string &path)
{
    const SmbLocation location = smb_location(folder);
    return location.share == share && location.path == path;
}

} // namespace

int main()
{
    // The folder's first part is the share.
    check(located("/", "", ""), "\"/\" is the NAS itself (its shares)");
    check(located("/Games", "Games", ""), "a share alone");
    check(located("/Games/PS5/", "Games", "PS5"), "a folder in a share");
    check(located("/Games/PS5/My Games", "Games", "PS5/My Games"), "a deeper folder, with a space");
    check(located("\\Games\\PS5", "Games", "PS5"), "Windows' backslashes");

    std::string domain, user;
    split_smb_user("WORKGROUP\\james", &domain, &user);
    check(domain == "WORKGROUP" && user == "james", "DOMAIN\\name splits", domain + " / " + user);
    split_smb_user("james", &domain, &user);
    check(domain.empty() && user == "james", "a plain name has no domain");

    check(smb_skipped_name(".hidden") && smb_skipped_name("@eaDir") &&
              smb_skipped_name("#recycle") && smb_skipped_name("$RECYCLE.BIN") &&
              smb_skipped_name("System Volume Information"),
          "hidden and NAS-internal names are left out");
    check(!smb_skipped_name("Astro Bot") && !smb_skipped_name("game.exfat"),
          "games are kept");

    // The connection's SMB settings.
    Connection connection;
    connection.type = ConnectionType::smb;
    connection.host = "192.168.1.20";
    connection.folder = "/Games/PS5";
    connection.username = "james";
    check(connection.default_port() == 445, "SMB's usual port is 445");
    check(connection.display_url() == "smb://james@192.168.1.20/Games/PS5/", "the address shown",
          connection.display_url());
    for (const char *typed : {"smb://192.168.1.20", "\\\\192.168.1.20", "192.168.1.20"})
    {
        std::string host = typed;
        const std::string error = normalise_host(&host);
        check(error.empty() && host == "192.168.1.20", std::string("address typed as ") + typed,
              error.empty() ? host : error);
    }

    // What failures say.
    std::string headline, detail;
    SmbFailure failure;
    failure.kind = SmbFailure::Kind::sign_in;
    describe_smb_failure(connection, failure, &headline, &detail);
    check(headline == "Wrong user name or password", "a refused sign-in", headline);
    failure.kind = SmbFailure::Kind::no_share;
    describe_smb_failure(connection, failure, &headline, &detail);
    check(detail.find("\"Games\"") != std::string::npos, "a missing share is named", detail);

    // Signing in where nothing listens: SMB is off.
    connection.host = "127.0.0.1";
    connection.port = free_port();
    {
        SmbSession session(connection, 3);
        std::vector<SmbEntry> entries;
        const bool ok = session.list("/Games/", &entries, &failure);
        check(!ok && failure.kind == SmbFailure::Kind::refused, "a closed port is \"SMB isn't answering\"",
              failure.detail);
    }
    // A name instead of an address is refused before connecting.
    connection.host = "nas.local";
    {
        SmbSession session(connection, 3);
        std::vector<std::string> shares;
        const bool ok = session.list_shares(&shares, &failure);
        check(!ok && failure.kind == SmbFailure::Kind::address, "a name isn't looked up");
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return g_failures == 0 ? 0 : 1;
}
