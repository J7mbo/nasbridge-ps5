#!/usr/bin/env python3
"""Install a folder onto the PS5 through the NASBridge installer payload.

Used by `make install`. Sends the installer ELF to the console's ELF loader,
waits for the installer to open its port, streams the folder (every file with
its SHA-256), and prints the installer's verdict. Wire format: see
tools/installer/installer.c.
"""

import argparse
import hashlib
import os
import socket
import struct
import sys
import time

ROOTS = {"homebrew": 0, "selftest": 1}
CHUNK = 1024 * 1024


def collect(folder):
    """Every regular file under `folder`, as (relative path, full path), sorted."""
    files = []
    for dirpath, dirnames, filenames in os.walk(folder):
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            if os.path.islink(full):
                sys.exit(f"error: symlinks are not supported: {full}")
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            files.append((rel, full))
    if not files:
        sys.exit(f"error: {folder} has no files")
    return files


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.digest()


def send_payload(host, port, elf):
    with open(elf, "rb") as f:
        data = f.read()
    try:
        with socket.create_connection((host, port), timeout=10) as s:
            s.sendall(data)
    except OSError as e:
        sys.exit(f"error: could not reach the ELF loader at {host}:{port} ({e}).\n"
                 "Is the PS5 on, jailbroken, and is the IP right?")


def connect_installer(host, port, wait_s=20):
    deadline = time.monotonic() + wait_s
    while True:
        try:
            return socket.create_connection((host, port), timeout=5)
        except OSError:
            if time.monotonic() > deadline:
                sys.exit(f"error: the installer didn't open port {port} on {host} "
                         f"within {wait_s}s. Check the PS5 for a notification.")
            time.sleep(0.5)


def read_reply(s):
    reply = b""
    try:
        while not reply.endswith(b"\n"):
            part = s.recv(1024)
            if not part:
                break
            reply += part
    except OSError:
        pass
    return reply.decode(errors="replace").strip()


def uninstall(args):
    """Removes /data/homebrew/<title ID> and the title's home-screen entry."""
    name = args.uninstall
    if args.installer:
        print(f"Sending installer to {args.host}:{args.loader_port}")
        send_payload(args.host, args.loader_port, args.installer)
    print(f"Uninstalling {name}: /data/homebrew/{name} and its home-screen entry")
    s = connect_installer(args.host, args.installer_port)
    s.settimeout(120)
    with s:
        name_b = name.encode()
        try:
            s.sendall(b"NBU1" + struct.pack("<BH", 0, len(name_b)) + name_b)
        except OSError:
            pass
        reply = read_reply(s)
    if reply.startswith("OK"):
        print(f"Uninstalled {name}")
        return 0
    print(f"error: {reply[4:] if reply.startswith('ERR ') else reply or 'no answer from the installer'}",
          file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", required=True)
    ap.add_argument("--loader-port", type=int, default=9021)
    ap.add_argument("--installer-port", type=int, default=9145)
    ap.add_argument("--installer", help="installer ELF to send first (omit if already running)")
    ap.add_argument("--root", choices=ROOTS, default="homebrew")
    ap.add_argument("--name", help="destination folder name (default: the folder's own name)")
    ap.add_argument("--uninstall", metavar="TITLE_ID",
                    help="remove this app instead: its folder in /data/homebrew, then its "
                         "home-screen entry")
    ap.add_argument("folder", nargs="?")
    args = ap.parse_args()
    # Docker gives no terminal, so stdout would be block-buffered and its
    # lines would show after an error printed to stderr.
    sys.stdout.reconfigure(line_buffering=True)
    if args.uninstall:
        return uninstall(args)
    if not args.folder:
        ap.error("the folder to install is required")

    folder = os.path.normpath(args.folder)
    name = args.name or os.path.basename(folder)
    files = collect(folder)
    total = sum(os.path.getsize(full) for _, full in files)

    if args.installer:
        print(f"Sending installer to {args.host}:{args.loader_port}")
        send_payload(args.host, args.loader_port, args.installer)

    dest = "/data/homebrew" if args.root == "homebrew" else "/data/nasbridge/selftest"
    print(f"Installing {len(files)} files ({total:,} bytes) to {dest}/{name}")

    s = connect_installer(args.host, args.installer_port)
    s.settimeout(120)
    with s:
        try:
            name_b = name.encode()
            s.sendall(b"NBI1" + struct.pack("<BH", ROOTS[args.root], len(name_b)) + name_b
                      + struct.pack("<I", len(files)))
            for rel, full in files:
                rel_b = rel.encode()
                s.sendall(struct.pack("<H", len(rel_b)) + rel_b
                          + struct.pack("<Q", os.path.getsize(full)) + sha256_of(full))
                with open(full, "rb") as f:
                    for block in iter(lambda: f.read(CHUNK), b""):
                        s.sendall(block)
            s.sendall(b"END1")
        except OSError:
            # The installer refused part-way (e.g. the app is open) and
            # closed the connection; its reason is waiting to be read.
            pass

        reply = b""
        try:
            while not reply.endswith(b"\n"):
                part = s.recv(1024)
                if not part:
                    break
                reply += part
        except OSError:
            pass

    reply = reply.decode(errors="replace").strip()
    if reply.startswith("OK"):
        print(f"Installed: {dest}/{name}")
        if args.root == "homebrew":
            print("Wait about 20 seconds before opening it, while ShadowMountPlus picks it up.")
        return 0
    print(f"error: install failed: {reply[4:] if reply.startswith('ERR') else reply or 'no reply'}",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
