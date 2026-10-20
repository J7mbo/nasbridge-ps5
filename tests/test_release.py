"""Tests for the release installer (`make release`): the installer with an app
packed inside it, built for this computer and run against a temporary folder,
with a stand-in for ShadowMountPlus's API. Run by `make test`.

It packs a small fake app with tools/release/pack.py, links it in with
tools/release/blob.S exactly as the release build does, and checks that the
installer installs it, updates it, refreshes the home-screen pictures, asks
ShadowMountPlus to scan, and refuses while the app is open."""

import filecmp
import http.server
import os
import shutil
import stat
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "build", "host", "release-test")
ZLIB = os.environ.get("NB_ZLIB_SRC", "/opt/ps5-homebrew-ui/.deps/native/zlib/zlib-1.3.2")
TITLE = "PPSB00001"
SMP_PORT = 10197
results = []


def check(ok, what, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {what}{f'  [{detail}]' if detail else ''}")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def make_app():
    """A small app folder: param.json, pictures, a 3 MB eboot.bin and nested files."""
    app = os.path.join(WORK, "app", TITLE)
    write(os.path.join(app, "sce_sys", "param.json"),
          b'{"titleId": "PPSB00001", "titleName": "Release Test"}\n')
    write(os.path.join(app, "sce_sys", "icon0.png"), b"new icon " * 100)
    write(os.path.join(app, "sce_sys", "pic0.dds"), b"new background " * 100)
    write(os.path.join(app, "eboot.bin"), bytes((i * 7919) % 251 for i in range(3 << 20)))
    write(os.path.join(app, "assets", "nested dir", "file.txt"), b"hello\n")
    return app


def build(app):
    """Packs the app and builds the host release installer around it."""
    blob = os.path.join(WORK, "app.nbz")
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "release", "pack.py"), app, blob],
                   check=True, capture_output=True)
    binary = os.path.join(WORK, "release-installer")
    zlib_srcs = [os.path.join(ZLIB, f"{n}.c")
                 for n in ("adler32", "crc32", "inffast", "inflate", "inftrees", "zutil")]
    subprocess.run(
        ["clang-18", "-O1", "-w", "-I" + ZLIB, "-c"] + zlib_srcs, check=True, cwd=WORK)
    subprocess.run(
        ["clang-18", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-D_DEFAULT_SOURCE",
         "-DINSTALLER_HOST_TEST", "-DINSTALLER_EMBEDDED", '-DNASBRIDGE_VERSION="9.9.9"',
         f'-DNB_BLOB="{blob}"', "-I" + ZLIB,
         os.path.join(ROOT, "tools", "installer", "installer.c"),
         os.path.join(ROOT, "tools", "installer", "sha256.c"),
         os.path.join(ROOT, "tools", "release", "blob.S")]
        + [os.path.join(WORK, f"{n}.o") for n in
           ("adler32", "crc32", "inffast", "inflate", "inftrees", "zutil")]
        + ["-o", binary], check=True)
    return binary


class FakeSmp(http.server.BaseHTTPRequestHandler):
    scans = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        FakeSmp.scans.append((self.path, body))
        reply = b'{"status": 0}'
        self.send_response(200)
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


def run(binary, prefix, running=None, smp=True):
    env = dict(os.environ, NB_TEST_PREFIX=prefix,
               NB_TEST_SMP_PORT=str(SMP_PORT if smp else SMP_PORT + 1))
    if running:
        env["NB_TEST_RUNNING"] = running
    return subprocess.run([binary], env=env, capture_output=True, text=True, timeout=60)


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, cmp.common_files, shallow=False)
    return not mismatch and not errors and all(
        same_tree(os.path.join(a, d), os.path.join(b, d)) for d in cmp.common_dirs)


def modes_open(tree):
    for dirpath, dirnames, filenames in os.walk(tree):
        for name in dirnames + filenames:
            if stat.S_IMODE(os.lstat(os.path.join(dirpath, name)).st_mode) != 0o777:
                return False
    return True


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    app = make_app()
    binary = build(app)
    prefix = os.path.join(WORK, "ps5")
    dest = os.path.join(prefix, "data", "homebrew", TITLE)
    meta = os.path.join(prefix, "user", "appmeta", TITLE)

    server = http.server.HTTPServer(("127.0.0.1", SMP_PORT), FakeSmp)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    # 1. A first install: every file, open to every user, and a scan asked for.
    r = run(binary, prefix)
    check(r.returncode == 0 and r.stdout.strip().splitlines()[-1].startswith("OK 5 "),
          "installs the packed app",
          (r.stdout + r.stderr).strip())
    check(os.path.isdir(dest) and same_tree(app, dest), "every file arrives identical")
    check(modes_open(dest), "every file and folder is open to every user (0777)")
    check(not os.path.exists(os.path.join(prefix, "data", "nasbridge", "staging")),
          "the staging folder is gone")
    check("[notify] NASBridge 9.9.9 installed" in r.stdout, "a first install says so",
          r.stdout.strip())
    check(FakeSmp.scans == [("/api/v1/scan", b"{}")], "ShadowMountPlus is asked to scan",
          str(FakeSmp.scans))

    # 2. An update: a file the new version doesn't have goes, and the home
    #    screen's own copies of the pictures are refreshed.
    write(os.path.join(dest, "old-only.txt"), b"from an older version")
    write(os.path.join(meta, "icon0.png"), b"old icon")
    write(os.path.join(meta, "pic0.dds"), b"old background")
    r = run(binary, prefix)
    check(r.returncode == 0 and same_tree(app, dest), "an update replaces the app",
          (r.stdout + r.stderr).strip())
    check("[notify] NASBridge updated to 9.9.9" in r.stdout, "an update says so", r.stdout.strip())
    check(filecmp.cmp(os.path.join(app, "sce_sys", "icon0.png"), os.path.join(meta, "icon0.png"),
                      shallow=False) and
          filecmp.cmp(os.path.join(app, "sce_sys", "pic0.dds"), os.path.join(meta, "pic0.dds"),
                      shallow=False),
          "the home screen's pictures are refreshed")

    # 3. Refused while the app is open, and nothing changes.
    write(os.path.join(dest, "marker.txt"), b"still here")
    r = run(binary, prefix, running=TITLE)
    check(r.returncode == 1 and "is open on the PS5" in r.stdout and
          os.path.exists(os.path.join(dest, "marker.txt")),
          "refuses while the app is open, leaving it alone", r.stdout.strip())

    # 4. Without ShadowMountPlus's API, it still installs.
    shutil.rmtree(dest)
    r = run(binary, prefix, smp=False)
    check(r.returncode == 0 and same_tree(app, dest), "installs without ShadowMountPlus's API",
          (r.stdout + r.stderr).strip())

    server.shutdown()
    print("ALL PASSED" if all(results) else "SOME FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
