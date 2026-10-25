// NASBridge - FTP requests to the NAS.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/ftp.hpp"

#include "concepts/smb.hpp"

#include <pthread.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
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

constexpr int kConnectSeconds = 5;
constexpr int kReceiveBuffer = 4 * 1024 * 1024; // as http.cpp's: lets the window grow
constexpr std::size_t kPiece = 1024 * 1024;     // stream(): one recv's most
constexpr std::size_t kReadAhead = 256 * 1024;  // read_range(): read at least this much
constexpr std::size_t kMaxListing = 64u * 1024u * 1024u;

void sleep_ms(int ms)
{
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

std::string lowered_ascii(std::string text)
{
    for (char &c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool contains(const std::string &text, const char *word)
{
    return lowered_ascii(text).find(word) != std::string::npos;
}

bool all_digits(const std::string &text)
{
    if (text.empty())
        return false;
    for (const char c : text)
        if (c < '0' || c > '9')
            return false;
    return true;
}

bool send_text(int fd, const std::string &text)
{
    std::size_t sent = 0;
    while (sent < text.size())
    {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, 0);
#endif
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// A connection to ip:port, or -1 with *error set. `seconds`: how long a
// read may wait.
int open_socket(const std::string &ip, int port, int seconds, int *error)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
    {
        *error = EINVAL;
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *error = errno;
        return -1;
    }
    const int buffer = kReceiveBuffer;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
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
        pollfd p{fd, POLLOUT, 0};
        int ready;
        do
            ready = ::poll(&p, 1, kConnectSeconds * 1000);
        while (ready < 0 && errno == EINTR);
        int result = 0;
        socklen_t length = sizeof(result);
        if (ready <= 0)
            result = ready == 0 ? ETIMEDOUT : errno;
        else if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length) != 0)
            result = errno;
        if (result != 0)
        {
            *error = result;
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    timeval wait{seconds, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
    return fd;
}

// Days since 1970-01-01 of a date (Howard Hinnant's days_from_civil).
long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// MLSD's "20261025081500" (UTC; maybe with ".123") as seconds since 1970.
std::string mlsd_time(const std::string &value)
{
    if (value.size() < 14 || !all_digits(value.substr(0, 14)))
        return value;
    const auto number = [&](std::size_t at, std::size_t length)
    { return std::atoi(value.substr(at, length).c_str()); };
    const long long days = days_from_civil(number(0, 4), static_cast<unsigned>(number(4, 2)),
                                           static_cast<unsigned>(number(6, 2)));
    return std::to_string(days * 86400 + number(8, 2) * 3600 + number(10, 2) * 60 + number(12, 2));
}

bool is_month(const std::string &word)
{
    static const char *const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                          "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::string low = lowered_ascii(word);
    for (const char *month : kMonths)
        if (low == month)
            return true;
    return false;
}

// The words of a line, and where each starts.
struct Word
{
    std::string text;
    std::size_t at;
    std::size_t end;
};

std::vector<Word> words_of(const std::string &line)
{
    std::vector<Word> words;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= line.size())
            break;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t')
            ++i;
        words.push_back({line.substr(start, i - start), start, i});
    }
    return words;
}

// The rest of a line after a word: one space, then the name (which may hold
// spaces of its own).
std::string name_after(const std::string &line, const Word &word)
{
    std::size_t at = word.end;
    if (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
        ++at;
    return line.substr(std::min(at, line.size()));
}

// The reply after a code: "227 Entering Passive Mode (...)" without "227 ".
std::string trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
}

// A short, safe copy of the NAS's words for a detail line.
std::string first_line(const std::string &text)
{
    const std::string line = trimmed(text.substr(0, text.find('\n')));
    return line.size() > 160 ? line.substr(0, 160) : line;
}

} // namespace

std::string ftp_user(const Connection &connection)
{
    return connection.username.empty() ? std::string("anonymous") : connection.username;
}

std::string ftp_password(const Connection &connection)
{
    return connection.username.empty() && connection.password.empty() ? std::string("nasbridge@")
                                                                      : connection.password;
}

std::string ftp_path(const std::string &path)
{
    std::string out = path.empty() || path.front() != '/' ? "/" + path : path;
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out;
}

bool parse_mlsd_line(const std::string &raw, FtpEntry *entry)
{
    const std::string line = trimmed(raw);
    const std::size_t space = line.find(' ');
    if (space == std::string::npos || space + 1 >= line.size())
        return false;
    *entry = FtpEntry{};
    entry->name = line.substr(space + 1);
    std::string type;
    std::size_t at = 0;
    const std::string facts = line.substr(0, space);
    while (at < facts.size())
    {
        std::size_t end = facts.find(';', at);
        if (end == std::string::npos)
            end = facts.size();
        const std::string fact = facts.substr(at, end - at);
        const std::size_t equals = fact.find('=');
        if (equals != std::string::npos)
        {
            const std::string key = lowered_ascii(fact.substr(0, equals));
            const std::string value = fact.substr(equals + 1);
            if (key == "type")
                type = lowered_ascii(value);
            else if (key == "size")
                entry->size = std::strtoull(value.c_str(), nullptr, 10);
            else if (key == "modify")
                entry->modified = mlsd_time(value);
        }
        at = end + 1;
    }
    // "cdir" and "pdir" are the folder itself and its parent; links and
    // devices ("OS.unix=symlink") are left out.
    if (type == "dir")
        entry->folder = true;
    else if (type != "file")
        return false;
    if (entry->folder)
        entry->size = 0;
    return entry->name != "." && entry->name != "..";
}

bool parse_list_line(const std::string &raw, FtpEntry *entry)
{
    const std::string line = trimmed(raw);
    const std::vector<Word> words = words_of(line);
    if (words.size() < 4)
        return false;
    *entry = FtpEntry{};
    // IIS's DOS style: "10-25-26  08:15AM  <DIR>  name" or "...  1234 name".
    if (words[0].text.size() >= 8 && std::isdigit(static_cast<unsigned char>(words[0].text[0])) &&
        words[0].text.find('-') != std::string::npos)
    {
        entry->folder = words[2].text == "<DIR>";
        if (!entry->folder && !all_digits(words[2].text))
            return false;
        entry->size = entry->folder ? 0 : std::strtoull(words[2].text.c_str(), nullptr, 10);
        entry->modified = words[0].text + " " + words[1].text;
        entry->name = words[3].at < line.size() ? line.substr(words[3].at) : ""; // after the padding
    }
    else
    {
        // Unix "ls -l": the type is the first letter, then the size and the
        // date come before the name: size, month, day, time or year. Owner
        // and group may each be missing, so the month is looked for.
        const char type = words[0].text[0];
        if (type != 'd' && type != '-')
            return false; // a link, a device, or "total 12"
        std::size_t month = 0;
        for (std::size_t i = 2; i + 2 < words.size(); ++i)
            if (is_month(words[i].text) && all_digits(words[i - 1].text) &&
                all_digits(words[i + 1].text) && words[i + 1].text.size() <= 2)
            {
                month = i;
                break;
            }
        if (month == 0 || month + 3 >= words.size())
            return false;
        entry->folder = type == 'd';
        entry->size = entry->folder ? 0 : std::strtoull(words[month - 1].text.c_str(), nullptr, 10);
        entry->modified = words[month].text + " " + words[month + 1].text + " " + words[month + 2].text;
        entry->name = name_after(line, words[month + 2]);
    }
    return !entry->name.empty() && entry->name != "." && entry->name != "..";
}

bool ftp_skipped_name(const std::string &name)
{
    return smb_skipped_name(name);
}

std::string ftp_failure_brief(const FtpFailure &failure)
{
    switch (failure.kind)
    {
    case FtpFailure::Kind::none:
        return "no problem";
    case FtpFailure::Kind::address:
        return "not an IP address";
    case FtpFailure::Kind::connect:
        return "couldn't reach the NAS";
    case FtpFailure::Kind::refused:
        return "FTP isn't answering";
    case FtpFailure::Kind::timeout:
        return "the NAS stopped answering";
    case FtpFailure::Kind::lost:
        return "the NAS closed the connection";
    case FtpFailure::Kind::sign_in:
        return "sign-in refused";
    case FtpFailure::Kind::busy:
        return "too many connections";
    case FtpFailure::Kind::tls:
        return "the NAS only allows FTPS";
    case FtpFailure::Kind::not_found:
        return "not found";
    case FtpFailure::Kind::denied:
        return "access denied";
    case FtpFailure::Kind::passive:
        return "no data connection";
    case FtpFailure::Kind::other:
        break;
    }
    return failure.detail.empty() ? std::string("FTP error") : failure.detail;
}

void describe_ftp_failure(const Connection &connection, const FtpFailure &failure,
                          std::string *headline, std::string *detail)
{
    const std::string where = connection.host + ":" + std::to_string(connection.effective_port());
    switch (failure.kind)
    {
    case FtpFailure::Kind::none:
        *headline = "Connected";
        detail->clear();
        return;
    case FtpFailure::Kind::address:
        *headline = "Enter the NAS's IP address";
        *detail = "NASBridge can't look up names yet: use the NAS's IPv4 address, "
                  "such as 192.168.1.20.";
        return;
    case FtpFailure::Kind::connect:
        *headline = "Can't reach the NAS";
        *detail = "Nothing answered at " + connection.host +
                  ". Check the address, and that the NAS and the PS5 are on the same network.";
        return;
    case FtpFailure::Kind::refused:
        *headline = "FTP isn't answering";
        *detail = "The NAS is there, but nothing answers on port " +
                  std::to_string(connection.effective_port()) +
                  ". Turn on FTP in the NAS's settings, or check the port (usually 21).";
        return;
    case FtpFailure::Kind::timeout:
        *headline = "The NAS stopped answering";
        *detail = "No reply from " + where + " in time. Check the NAS is awake and try again.";
        return;
    case FtpFailure::Kind::lost:
        *headline = "The NAS closed the connection";
        *detail = failure.detail.empty()
                      ? "The NAS's FTP server hung up. Check it's running and try again."
                      : "The NAS's FTP server hung up: \"" + failure.detail + "\".";
        return;
    case FtpFailure::Kind::sign_in:
        *headline = "Wrong user name or password";
        *detail = "The NAS didn't accept the sign-in. Check both, or leave them empty to "
                  "sign in as anonymous if the NAS allows it.";
        return;
    case FtpFailure::Kind::busy:
        *headline = "Too many connections";
        *detail = "The NAS's FTP server turned NASBridge away because too many connections "
                  "are open. Close other FTP apps, or raise the NAS's limit on connections "
                  "per user or per address.";
        return;
    case FtpFailure::Kind::tls:
        *headline = "The NAS only allows FTPS";
        *detail = "NASBridge speaks plain FTP, without encryption. In the NAS's FTP settings, "
                  "allow plain FTP, or use SMB or WebDAV instead.";
        return;
    case FtpFailure::Kind::not_found:
        *headline = "Folder not found";
        *detail = "The NAS has no folder " + connection.folder +
                  " for this user. Pick the games folder with the folder button.";
        return;
    case FtpFailure::Kind::denied:
        *headline = "Not allowed to read this folder";
        *detail = "The NAS accepted the sign-in, but this user may not read " +
                  connection.folder + ". Give the user read access on the NAS.";
        return;
    case FtpFailure::Kind::passive:
        *headline = "Can't open a data connection";
        *detail = "Signed in, but the NAS's passive-mode connection couldn't be opened. "
                  "Check passive mode is on in the NAS's FTP settings and that its port "
                  "range isn't blocked.";
        return;
    case FtpFailure::Kind::other:
        break;
    }
    *headline = "Couldn't read the NAS";
    *detail = failure.detail.empty() ? std::string("Something went wrong talking to ") + where + "."
                                     : failure.detail;
}

FtpSession::FtpSession(const Connection &connection, int seconds)
    : connection_(connection), seconds_(seconds)
{
}

FtpSession::~FtpSession()
{
    disconnect();
}

void FtpSession::disconnect()
{
    const int data = data_.exchange(-1);
    if (data >= 0)
        ::close(data);
    const int control = control_.exchange(-1);
    if (control >= 0)
    {
        send_text(control, "QUIT\r\n"); // a courtesy: not waited for
        ::close(control);
    }
    pending_.clear();
}

void FtpSession::cut()
{
    const int data = data_.load();
    if (data >= 0)
        ::shutdown(data, SHUT_RDWR);
    const int control = control_.load();
    if (control >= 0)
        ::shutdown(control, SHUT_RDWR);
}

void FtpSession::network(int error, FtpFailure *why)
{
    *why = FtpFailure{};
    switch (error)
    {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ETIMEDOUT:
        why->kind = FtpFailure::Kind::timeout;
        break;
    case ECONNREFUSED:
        why->kind = FtpFailure::Kind::refused;
        break;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EHOSTDOWN:
    case ENETDOWN:
        why->kind = FtpFailure::Kind::connect;
        break;
    default:
        why->kind = FtpFailure::Kind::lost;
        if (error != 0)
            why->detail = std::strerror(error);
        break;
    }
    disconnect(); // mid-reply: start again next time
}

void FtpSession::refused(int code, const std::string &text, FtpFailure *why)
{
    *why = FtpFailure{};
    why->code = code;
    why->detail = first_line(text);
    using Kind = FtpFailure::Kind;
    // "530 Non-anonymous sessions must use encryption" (vsftpd),
    // "534 Policy requires SSL" (ProFTPD), "550 SSL/TLS required" (pure-ftpd).
    if ((code == 530 || code == 534 || code == 550 || code == 521) &&
        (contains(text, "tls") || contains(text, "ssl") || contains(text, "encrypt")))
        why->kind = Kind::tls;
    else if (code == 530 || code == 332)
        why->kind = Kind::sign_in;
    else if (code == 421)
        why->kind = contains(text, "many") || contains(text, "limit") || contains(text, "max")
                        ? Kind::busy
                        : Kind::lost;
    else if (code == 550 || code == 450 || code == 553)
        why->kind = contains(text, "denied") || contains(text, "ermission") ? Kind::denied
                                                                              : Kind::not_found;
    else if (code == 425)
        why->kind = Kind::passive;
    else if (code == 426)
        why->kind = Kind::lost;
    else
        why->kind = Kind::other;
    if (code == 421)
        disconnect(); // the NAS is closing it
}

bool FtpSession::read_line(std::string *line, FtpFailure *why)
{
    while (true)
    {
        const std::size_t nl = pending_.find('\n');
        if (nl != std::string::npos)
        {
            *line = trimmed(pending_.substr(0, nl));
            pending_.erase(0, nl + 1);
            return true;
        }
        if (pending_.size() > 64 * 1024)
        {
            network(EPROTO, why);
            return false;
        }
        char chunk[2048];
        const ssize_t n = ::recv(control_.load(), chunk, sizeof(chunk), 0);
        if (n > 0)
        {
            pending_.append(chunk, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        network(n == 0 ? 0 : errno, why);
        return false;
    }
}

bool FtpSession::reply(int *code, std::string *text, FtpFailure *why)
{
    std::string line;
    if (!read_line(&line, why))
        return false;
    // Lines before a reply that don't start with a code are skipped (some
    // servers' banners); "123-" opens a reply of several lines, "123 " ends it.
    while (line.size() < 3 || !all_digits(line.substr(0, 3)))
        if (!read_line(&line, why))
            return false;
    *code = std::atoi(line.substr(0, 3).c_str());
    *text = line.size() > 4 ? line.substr(4) : "";
    if (line.size() > 3 && line[3] == '-')
    {
        const std::string end = line.substr(0, 3) + " ";
        while (true)
        {
            if (!read_line(&line, why))
                return false;
            *text += "\n" + line;
            if (line.compare(0, 4, end) == 0)
                break;
        }
    }
    return true;
}

bool FtpSession::command(const std::string &line, int *code, std::string *text, FtpFailure *why)
{
    if (!send_text(control_.load(), line + "\r\n"))
    {
        network(errno, why);
        return false;
    }
    // A transfer cut off early may get a second last word from some servers
    // ("426", then "226"): one left over is skipped, as no other command
    // answers with those.
    const bool transfer = line.compare(0, 4, "RETR") == 0 || line.compare(0, 4, "LIST") == 0 ||
                          line.compare(0, 4, "MLSD") == 0;
    do
        if (!reply(code, text, why))
            return false;
    while (!transfer && (*code == 225 || *code == 226 || *code == 426));
    return true;
}

bool FtpSession::sign_in(FtpFailure *why)
{
    *why = FtpFailure{};
    if (control_.load() >= 0)
        return true;
    in_addr address{};
    if (::inet_pton(AF_INET, connection_.host.c_str(), &address) != 1)
    {
        why->kind = FtpFailure::Kind::address;
        return false;
    }
    int error = 0;
    const int fd = open_socket(connection_.host, connection_.effective_port(), seconds_, &error);
    if (fd < 0)
    {
        network(error, why);
        return false;
    }
    control_.store(fd);
    pending_.clear();
    int code = 0;
    std::string text;
    // The greeting; "120 ready in a few minutes" comes before the real one.
    do
        if (!reply(&code, &text, why))
            return false;
    while (code == 120);
    if (code != 220)
    {
        refused(code, text, why);
        disconnect();
        return false;
    }
    if (!command("USER " + ftp_user(connection_), &code, &text, why))
        return false;
    if (code == 331 && !command("PASS " + ftp_password(connection_), &code, &text, why))
        return false;
    if (code != 230 && code != 202)
    {
        refused(code, text, why);
        if (why->kind == FtpFailure::Kind::other)
            why->kind = FtpFailure::Kind::sign_in;
        disconnect();
        return false;
    }
    if (!command("TYPE I", &code, &text, why))
        return false;
    if (code != 200)
    {
        refused(code, text, why);
        disconnect();
        return false;
    }
    // What the server has: MLSD (it says "MLST"), and UTF-8 names.
    if (!command("FEAT", &code, &text, why))
        return false;
    mlsd_ = code == 211 && contains(text, "\n mlst");
    if (code == 211 && contains(text, "utf8") && !command("OPTS UTF8 ON", &code, &text, why))
        return false;
    return true;
}

int FtpSession::open_data(FtpFailure *why)
{
    int code = 0;
    std::string text;
    int port = 0;
    // EPSV: "229 Entering Extended Passive Mode (|||6446|)".
    if (epsv_)
    {
        if (!command("EPSV", &code, &text, why))
            return -1;
        const std::size_t bars = text.find("|||");
        if (code == 229 && bars != std::string::npos)
            port = std::atoi(text.c_str() + bars + 3);
        else if (code >= 500 && code <= 504)
            epsv_ = false; // PASV from here on
        else
        {
            refused(code, text, why);
            if (why->kind == FtpFailure::Kind::other)
                why->kind = FtpFailure::Kind::passive;
            return -1;
        }
    }
    // PASV: "227 Entering Passive Mode (192,168,1,20,25,46)". The address is
    // the one already signed in to: a NAS behind a router may give one the
    // PS5 can't reach, and the ports are what matter.
    if (port == 0)
    {
        if (!command("PASV", &code, &text, why))
            return -1;
        if (code != 227)
        {
            refused(code, text, why);
            if (why->kind == FtpFailure::Kind::other)
                why->kind = FtpFailure::Kind::passive;
            return -1;
        }
        std::size_t at = text.find_first_of("0123456789");
        int numbers[6] = {};
        int count = 0;
        while (at != std::string::npos && count < 6)
        {
            numbers[count++] = std::atoi(text.c_str() + at);
            at = text.find_first_not_of("0123456789", at);
            if (at != std::string::npos)
                at = text.find_first_of("0123456789", at);
        }
        if (count == 6)
            port = numbers[4] * 256 + numbers[5];
    }
    if (port <= 0 || port > 65535)
    {
        *why = FtpFailure{};
        why->kind = FtpFailure::Kind::passive;
        why->detail = first_line(text);
        return -1;
    }
    int error = 0;
    const int fd = open_socket(connection_.host, port, seconds_, &error);
    if (fd < 0)
    {
        *why = FtpFailure{};
        why->kind = FtpFailure::Kind::passive;
        why->detail = std::string("port ") + std::to_string(port) + ": " + std::strerror(error);
        return -1;
    }
    data_.store(fd);
    return fd;
}

int FtpSession::begin(const std::string &line, std::uint64_t offset, bool *finished,
                      FtpFailure *why)
{
    *finished = false;
    const int fd = open_data(why);
    if (fd < 0)
        return -1;
    int code = 0;
    std::string text;
    // REST goes just before the transfer: some servers forget it after
    // another command (PASV among them).
    if (offset > 0)
    {
        if (!command("REST " + std::to_string(offset), &code, &text, why))
        {
            if (data_.exchange(-1) >= 0)
                ::close(fd);
            return -1;
        }
        if (code != 350)
        {
            if (data_.exchange(-1) >= 0)
                ::close(fd);
            refused(code, text, why);
            why->kind = FtpFailure::Kind::other;
            why->detail = "The NAS can't start a file part way (" + first_line(text) + ")";
            return -1;
        }
    }
    if (!command(line, &code, &text, why))
    {
        if (data_.exchange(-1) >= 0)
            ::close(fd);
        return -1;
    }
    if (code == 125 || code == 150)
        return fd;
    if (code == 226 || code == 250)
    {
        // Already done (an empty listing, on some servers): read what's there.
        *finished = true;
        return fd;
    }
    if (data_.exchange(-1) >= 0)
        ::close(fd);
    refused(code, text, why);
    return -1;
}

bool FtpSession::finish(int data, FtpFailure *why)
{
    if (data_.exchange(-1) >= 0)
        ::close(data);
    int code = 0;
    std::string text;
    if (!reply(&code, &text, why))
        return false;
    if (code == 226 || code == 250)
        return true;
    refused(code, text, why);
    return false;
}

void FtpSession::abandon(int data)
{
    // Closing the data connection mid-file ends the transfer; the NAS then
    // says so ("426"), or that it had finished ("226") if it sent it all.
    if (data_.exchange(-1) >= 0)
        ::close(data);
    int code = 0;
    std::string text;
    FtpFailure ignored;
    if (control_.load() >= 0 && (!reply(&code, &text, &ignored) || code == 421))
        disconnect(); // 421: the NAS is closing it
}

bool FtpSession::list(const std::string &folder, std::vector<FtpEntry> *entries, FtpFailure *why)
{
    entries->clear();
    if (!sign_in(why))
        return false;
    const std::string path = ftp_path(folder);
    int code = 0;
    std::string text;
    bool finished = false;
    int fd = -1;
    bool machine = mlsd_;
    if (machine)
    {
        fd = begin("MLSD " + path, 0, &finished, why);
        if (fd < 0 && why->code >= 500 && why->code <= 502 && control_.load() >= 0)
        {
            mlsd_ = machine = false; // it said MLST, but doesn't list with it
        }
        else if (fd < 0)
            return false;
    }
    if (!machine)
    {
        // LIST takes ls options and patterns on some servers, so the folder
        // is entered instead of named.
        if (!command("CWD " + path, &code, &text, why))
            return false;
        if (code != 250 && code != 200)
        {
            refused(code, text, why);
            if (why->kind == FtpFailure::Kind::other)
                why->kind = FtpFailure::Kind::not_found;
            return false;
        }
        fd = begin("LIST", 0, &finished, why);
        if (fd < 0)
            return false;
    }
    std::string body;
    char chunk[16384];
    while (true)
    {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0)
        {
            body.append(chunk, static_cast<std::size_t>(n));
            if (body.size() > kMaxListing)
            {
                abandon(fd);
                *why = FtpFailure{};
                why->kind = FtpFailure::Kind::other;
                why->detail = "The folder's listing is too long";
                return false;
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            const int error = errno;
            if (data_.exchange(-1) >= 0)
                ::close(fd);
            network(error, why);
            return false;
        }
        break;
    }
    if (finished)
    {
        if (data_.exchange(-1) >= 0)
            ::close(fd);
    }
    else if (!finish(fd, why))
        return false;
    std::size_t at = 0;
    while (at < body.size())
    {
        std::size_t end = body.find('\n', at);
        if (end == std::string::npos)
            end = body.size();
        FtpEntry entry;
        const std::string line = body.substr(at, end - at);
        if ((machine ? parse_mlsd_line(line, &entry) : parse_list_line(line, &entry)) &&
            !ftp_skipped_name(entry.name))
            entries->push_back(std::move(entry));
        at = end + 1;
    }
    return true;
}

bool FtpSession::stream(const std::string &file, std::uint64_t offset, std::uint64_t *size,
                        const std::function<bool(const char *data, std::size_t length)> &sink,
                        FtpFailure *why)
{
    if (!sign_in(why))
        return false;
    const std::string path = ftp_path(file);
    int code = 0;
    std::string text;
    if (!command("SIZE " + path, &code, &text, why))
        return false;
    if (code == 213)
    {
        if (size != nullptr)
            *size = std::strtoull(text.c_str(), nullptr, 10);
    }
    else if (code == 550 || code == 450)
    {
        refused(code, text, why);
        return false;
    }
    bool finished = false;
    const int fd = begin("RETR " + path, offset, &finished, why);
    if (fd < 0)
        return false;
    std::vector<char> buffer(kPiece);
    while (true)
    {
        const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n > 0)
        {
            if (!sink(buffer.data(), static_cast<std::size_t>(n)))
            {
                abandon(fd);
                *why = FtpFailure{};
                return false;
            }
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            const int error = errno;
            if (data_.exchange(-1) >= 0)
                ::close(fd);
            network(error, why);
            return false;
        }
        break;
    }
    if (finished)
    {
        if (data_.exchange(-1) >= 0)
            ::close(fd);
        return true;
    }
    return finish(fd, why);
}

bool FtpSession::read_file(const std::string &file, std::size_t max, std::string *out,
                           bool *too_big, FtpFailure *why)
{
    *too_big = false;
    out->clear();
    std::string data;
    std::uint64_t size = 0;
    const bool ok = stream(
        file, 0, &size,
        [&](const char *piece, std::size_t length)
        {
            if (size > max || data.size() + length > max)
            {
                *too_big = true;
                return false;
            }
            data.append(piece, length);
            return true;
        },
        why);
    if (ok)
        *out = std::move(data);
    return ok;
}

bool FtpSession::read_range(const std::string &file, std::uint64_t offset, std::size_t length,
                            std::string *out, FtpFailure *why)
{
    out->clear();
    *why = FtpFailure{};
    if (file == ahead_path_ && offset >= ahead_at_ && offset + length <= ahead_at_ + ahead_.size())
    {
        out->assign(ahead_, static_cast<std::size_t>(offset - ahead_at_), length);
        return true;
    }
    ahead_path_.clear();
    ahead_.clear();
    if (!sign_in(why))
        return false;
    bool finished = false;
    const int fd = begin("RETR " + ftp_path(file), offset, &finished, why);
    if (fd < 0)
        return false;
    const std::size_t want = std::max(length, kReadAhead);
    std::string data(want, '\0');
    std::size_t got = 0;
    bool ended = false;
    while (got < want)
    {
        const ssize_t n = ::recv(fd, &data[got], want - got, 0);
        if (n > 0)
        {
            got += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
        {
            const int error = errno;
            if (data_.exchange(-1) >= 0)
                ::close(fd);
            network(error, why);
            return false;
        }
        ended = true;
        break;
    }
    if (finished)
    {
        if (data_.exchange(-1) >= 0)
            ::close(fd);
    }
    else if (ended)
    {
        if (!finish(fd, why))
            return false;
    }
    else
        abandon(fd);
    data.resize(got);
    if (got < length)
    {
        why->kind = FtpFailure::Kind::other;
        why->detail = "The file is shorter than expected";
        return false;
    }
    out->assign(data, 0, length);
    ahead_path_ = file;
    ahead_at_ = offset;
    ahead_ = std::move(data);
    return true;
}

namespace
{

// One walk's shared state: the folders still to list, and the answers.
struct Walk
{
    std::mutex lock;
    std::deque<std::string> waiting; // paths, "/Games/PS5/Game" form
    int busy = 0;                    // listings in progress
    bool stop = false;               // a listing failed, or `found` said stop
    bool over = false;               // everything is listed
    struct Answer
    {
        std::string path;
        std::vector<FtpEntry> entries;
    };
    std::vector<Answer> answered;
    FtpFailure failure; // the first failed listing's
    std::string failed_path;
};

struct Lane
{
    Walk *walk;
    FtpSession *session;
    bool own; // made for the walk (signed in by the lane, deleted after)
    bool done = false; // the lane's thread has finished
    pthread_t thread{};
};

void *lane_main(void *argument)
{
    Lane *lane = static_cast<Lane *>(argument);
    Walk *walk = lane->walk;
    FtpFailure why;
    // A lane the NAS turns away (too many connections) just isn't used.
    if (lane->own && !lane->session->sign_in(&why))
    {
        std::lock_guard<std::mutex> hold(walk->lock);
        lane->done = true;
        return nullptr;
    }
    while (true)
    {
        std::string path;
        bool got = false;
        {
            std::lock_guard<std::mutex> hold(walk->lock);
            if (walk->stop || walk->over)
            {
                lane->done = true;
                return nullptr;
            }
            if (!walk->waiting.empty())
            {
                path = std::move(walk->waiting.front());
                walk->waiting.pop_front();
                ++walk->busy;
                got = true;
            }
        }
        if (!got)
        {
            sleep_ms(2);
            continue;
        }
        Walk::Answer answer;
        answer.path = path;
        const bool ok = lane->session->list(path + "/", &answer.entries, &why);
        std::lock_guard<std::mutex> hold(walk->lock);
        --walk->busy;
        if (!ok)
        {
            if (!walk->stop && walk->failure.kind == FtpFailure::Kind::none)
            {
                walk->failure = why;
                walk->failed_path = path;
            }
            walk->stop = true;
            lane->done = true;
            return nullptr;
        }
        walk->answered.push_back(std::move(answer));
    }
}

bool start_lane(Lane *lane)
{
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1024 * 1024); // as the app's other threads
    const int created = pthread_create(&lane->thread, &attributes, &lane_main, lane);
    pthread_attr_destroy(&attributes);
    return created == 0;
}

} // namespace

bool FtpSession::walk(
    const std::string &folder, int connections,
    const std::function<bool(const std::string &inside, const std::vector<FtpEntry> &entries)> &found,
    FtpFailure *why)
{
    if (!sign_in(why))
        return false;
    connections = std::max(connections, 1);
    const std::string top = ftp_path(folder);
    const std::size_t skip = top == "/" ? 1 : top.size() + 1;
    Walk walk;
    walk.waiting.push_back(top == "/" ? "" : top);
    // Lane 0 is this session's own sign-in; the others are made for the walk
    // once there's more than one folder to list (a small folder never needs
    // them), and signed out after it.
    std::vector<std::unique_ptr<Lane>> lanes;
    std::vector<std::unique_ptr<FtpSession>> extra;
    lanes.push_back(std::make_unique<Lane>(Lane{&walk, this, false}));
    if (!start_lane(lanes[0].get()))
    {
        *why = FtpFailure{};
        why->kind = FtpFailure::Kind::other;
        why->detail = "Couldn't start listing";
        return false;
    }
    bool stopped = false;
    bool more = connections > 1;
    while (true)
    {
        std::vector<Walk::Answer> answers;
        bool add_lane = false;
        {
            std::lock_guard<std::mutex> hold(walk.lock);
            answers.swap(walk.answered);
            add_lane = more && !walk.stop && walk.waiting.size() > 1;
        }
        if (add_lane)
        {
            extra.push_back(std::make_unique<FtpSession>(connection_, seconds_));
            lanes.push_back(std::make_unique<Lane>(Lane{&walk, extra.back().get(), true}));
            if (!start_lane(lanes.back().get()))
            {
                lanes.pop_back();
                extra.pop_back();
                more = false;
            }
            else if (static_cast<int>(lanes.size()) >= connections)
                more = false;
        }
        for (Walk::Answer &answer : answers)
        {
            if (stopped)
                break;
            std::vector<std::string> folders;
            for (const FtpEntry &entry : answer.entries)
                if (entry.folder)
                    folders.push_back(answer.path + "/" + entry.name);
            const std::string inside =
                answer.path.size() > skip ? answer.path.substr(skip) + "/" : std::string();
            if (!found(inside, answer.entries))
                stopped = true;
            std::lock_guard<std::mutex> hold(walk.lock);
            if (stopped)
                walk.stop = true;
            else
                for (std::string &path : folders)
                    walk.waiting.push_back(std::move(path));
        }
        if (stopped)
        {
            // Cut the listings in progress off rather than wait for them.
            for (std::size_t i = 1; i < lanes.size(); ++i)
                lanes[i]->session->cut();
            cut();
            break;
        }
        {
            std::lock_guard<std::mutex> hold(walk.lock);
            bool lanes_left = false;
            for (const auto &lane : lanes)
                lanes_left = lanes_left || !lane->done;
            if (walk.answered.empty() && walk.busy == 0 &&
                (walk.stop || walk.waiting.empty() || !lanes_left))
            {
                if (!walk.stop && !walk.waiting.empty() &&
                    walk.failure.kind == FtpFailure::Kind::none)
                {
                    walk.failure.kind = FtpFailure::Kind::other; // no sign-in left to list with
                    walk.failed_path = walk.waiting.front();
                }
                walk.over = true;
                break;
            }
        }
        if (answers.empty())
            sleep_ms(2);
    }
    for (const auto &lane : lanes)
        pthread_join(lane->thread, nullptr);
    // The walk's own sign-ins go; this one stays (unless it was cut off).
    extra.clear();
    if (stopped)
    {
        disconnect();
        *why = FtpFailure{};
        return false;
    }
    if (walk.failure.kind != FtpFailure::Kind::none)
    {
        *why = walk.failure;
        if (why->detail.empty())
            why->detail = "Couldn't list " + walk.failed_path;
        return false;
    }
    *why = FtpFailure{};
    return true;
}

} // namespace hui::concepts
