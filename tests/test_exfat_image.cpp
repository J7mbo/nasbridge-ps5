// NASBridge - Laying out a folder game as an exFAT image (exfat_image.cpp):
// images made from a few trees are read back with NASBridge's own reader,
// file by file, and by fsck.exfat when this computer has it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/exfat_image.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
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

// A file's contents in these tests: made from its path, so every file differs.
std::string contents(const ImageFile &file)
{
    std::string data(static_cast<std::size_t>(file.size), '\0');
    std::uint32_t x = 2166136261u;
    for (const char c : file.path)
        x = (x ^ static_cast<unsigned char>(c)) * 16777619u;
    for (char &c : data)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x);
    }
    return data;
}

// Writes the image as the copy helper would: piece by piece, front to back.
bool write_image(const ExfatImage &image, const std::vector<ImageFile> &files, const std::string &path)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    std::uint64_t at = 0;
    bool ok = true;
    const std::string zeros(1 << 20, '\0');
    for (const ImagePiece &piece : image.pieces)
    {
        std::string data;
        if (piece.kind == ImagePiece::Kind::bytes)
            data = piece.data;
        else if (piece.kind == ImagePiece::Kind::file)
            data = contents(files[piece.file]);
        if (piece.kind == ImagePiece::Kind::zeros)
        {
            for (std::uint64_t left = piece.length; left > 0 && ok;)
            {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, zeros.size()));
                ok = ::write(fd, zeros.data(), n) == static_cast<ssize_t>(n);
                left -= n;
            }
        }
        else
        {
            ok = ok && data.size() == piece.length &&
                 ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
        }
        at += piece.length;
    }
    ::close(fd);
    return ok && at == image.size;
}

ExfatReader::ReadAt reader_for(const std::string &path)
{
    return [path](std::uint64_t offset, std::size_t length, std::string *out)
    {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        out->assign(length, '\0');
        const ssize_t n = ::pread(fd, &(*out)[0], length, static_cast<off_t>(offset));
        ::close(fd);
        return n == static_cast<ssize_t>(length);
    };
}

// The file at `path` in the image, read through NASBridge's reader.
bool read_back(const std::string &image, const std::string &path, std::string *data)
{
    ExfatReader reader(reader_for(image));
    reader.set_limits(64u << 20, 1 << 20);
    std::string why;
    if (!reader.open(&why))
        return false;
    std::vector<ExfatFile> entries;
    if (!reader.list_root(&entries, &why))
        return false;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t slash = path.find('/', start);
        const std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const ExfatFile *entry = find_entry(entries, name.c_str());
        if (entry == nullptr)
            return false;
        if (slash == std::string::npos)
            return reader.read(*entry, 64u << 20, data, &why);
        const ExfatFile folder = *entry;
        if (!reader.list(folder, &entries, &why))
            return false;
        start = slash + 1;
    }
}

bool have_fsck()
{
    return std::system("command -v fsck.exfat >/dev/null 2>&1") == 0;
}

// Makes, writes, checks and reads back one game.
void round_trip(const std::string &work, const std::string &what, const std::vector<std::string> &folders,
                const std::vector<ImageFile> &files)
{
    ExfatImage image;
    std::string why;
    if (!plan_exfat_image(folders, files, "PPSA01234", &image, &why))
    {
        check(false, what + ": laid out", why);
        return;
    }
    std::uint64_t total = 0;
    for (const ImagePiece &piece : image.pieces)
        total += piece.length;
    check(total == image.size && image.size % 65536 == 0, what + ": the pieces fill the image exactly");
    const std::string path = work + "/game.exfat";
    check(write_image(image, files, path), what + ": written front to back");
    why.clear();
    check(check_exfat_image(reader_for(path), folders, files, &why), what + ": reads back as the game", why);
    bool same = true;
    std::string bad;
    for (const ImageFile &file : files)
    {
        std::string data;
        if (!read_back(path, file.path, &data) || data != contents(file))
        {
            same = false;
            bad = file.path;
            break;
        }
    }
    check(same, what + ": every file reads back identical", bad);
    if (have_fsck())
    {
        const int rc = std::system(("fsck.exfat -n \"" + path + "\" >\"" + work + "/fsck.txt\" 2>&1").c_str());
        std::string log;
        if (FILE *f = std::fopen((work + "/fsck.txt").c_str(), "r"))
        {
            char line[256];
            while (std::fgets(line, sizeof(line), f))
                log += line;
            std::fclose(f);
        }
        check(rc == 0, what + ": fsck.exfat finds nothing wrong", rc == 0 ? "" : log);
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const std::string work = argv[1];
    std::system(("rm -rf \"" + work + "\" && mkdir -p \"" + work + "\"").c_str());
    if (!have_fsck())
        std::printf("note: no fsck.exfat here; images are checked by NASBridge's reader only\n");

    // A small game: sce_sys, an empty file, a file of exactly one cluster, a
    // long name, a name in Japanese, an empty folder.
    round_trip(work, "a small game",
               {"sce_sys", "data", "data/empty folder", "data/sub"},
               {{"eboot.bin", 300000},
                {"sce_sys/param.json", 900},
                {"sce_sys/icon0.png", 70000},
                {"data/empty.dat", 0},
                {"data/one cluster.bin", 65536},
                {"data/sub/a file with a rather long name that takes several entries.bin", 5},
                {"data/sub/\xe3\x83\x87\xe3\x83\xbc\xe3\x82\xbf.pak", 131073}});

    // A folder of 5,000 files (several clusters of entries), files at the root.
    {
        std::vector<ImageFile> files{{"eboot.bin", 1000}, {"sce_sys/param.json", 100}};
        for (int i = 0; i < 5000; ++i)
            files.push_back({"assets/file" + std::to_string(i) + ".bin", static_cast<std::uint64_t>(i % 7) * 1000});
        round_trip(work, "a folder of 5,000 files", {}, files);
    }

    // The same list always makes the same image, whatever order it's in (a
    // resumed copy relies on it).
    {
        const std::vector<ImageFile> files{{"b.bin", 10}, {"a/c.bin", 20}, {"sce_sys/param.json", 30}};
        ExfatImage one, two;
        std::string why;
        plan_exfat_image({}, files, "PPSA01234", &one, &why);
        std::vector<ImageFile> reversed(files.rbegin(), files.rend());
        plan_exfat_image({"a"}, reversed, "PPSA01234", &two, &why);
        bool same = one.size == two.size && one.pieces.size() == two.pieces.size();
        for (std::size_t i = 0; same && i < one.pieces.size(); ++i)
            same = one.pieces[i].kind == two.pieces[i].kind && one.pieces[i].length == two.pieces[i].length &&
                   one.pieces[i].data == two.pieces[i].data &&
                   (one.pieces[i].kind != ImagePiece::Kind::file ||
                    files[one.pieces[i].file].path == reversed[two.pieces[i].file].path);
        check(same, "the same game always lays out the same");
    }

    // Spare room: 64 MiB for a small game.
    {
        ExfatImage image;
        std::string why;
        plan_exfat_image({}, {{"eboot.bin", 1000}}, "", &image, &why);
        check(image.free_bytes == (64u << 20), "a small image keeps 64 MiB free inside it");
    }

    // A big game: laid out without reading anything (sizes only).
    {
        std::vector<ImageFile> files;
        for (int i = 0; i < 20; ++i)
            files.push_back({"data/chunk" + std::to_string(i) + ".pak", 5000000000ull});
        ExfatImage image;
        std::string why;
        const bool ok = plan_exfat_image({}, files, "PPSA01234", &image, &why);
        const std::uint64_t data = 20 * 5000000000ull;
        check(ok && image.size > data && image.size < data + data / 100 + (64u << 20),
              "a 100 GB game is laid out, a little over its size",
              std::to_string(image.size));
    }

    // What exFAT can't hold is refused before anything is copied.
    {
        ExfatImage image;
        std::string why;
        check(!plan_exfat_image({}, {{"Data/a.bin", 1}, {"data/b.bin", 1}}, "", &image, &why) &&
                  why.find("upper and lower case") != std::string::npos,
              "two folders differing only in case are refused", why);
        why.clear();
        check(!plan_exfat_image({}, {{"a?.bin", 1}}, "", &image, &why) && !why.empty(),
              "a name with a character exFAT doesn't allow is refused", why);
        why.clear();
        check(!plan_exfat_image({}, {{std::string("bad\xff.bin"), 1}}, "", &image, &why) && !why.empty(),
              "a name that isn't UTF-8 is refused", why);
    }

    // A changed image is caught by the check.
    {
        const std::vector<ImageFile> files{{"eboot.bin", 10}, {"sce_sys/param.json", 20}};
        ExfatImage image;
        std::string why;
        plan_exfat_image({}, files, "", &image, &why);
        write_image(image, files, work + "/other.exfat");
        const std::vector<ImageFile> more{{"eboot.bin", 10}, {"sce_sys/param.json", 20}, {"extra.bin", 1}};
        check(!check_exfat_image(reader_for(work + "/other.exfat"), {}, more, &why),
              "an image missing a file fails the check", why);
        const std::vector<ImageFile> sized{{"eboot.bin", 11}, {"sce_sys/param.json", 20}};
        why.clear();
        check(!check_exfat_image(reader_for(work + "/other.exfat"), {}, sized, &why),
              "an image with a file of the wrong size fails the check", why);
    }

    std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
