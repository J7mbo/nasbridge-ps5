"""A stand-in PS5 ELF loader, for testing NASBridge's delete helper on a
computer.

    python3 tests/fake_loader.py <loader-port> <helper-binary> <helper-port> \
        <prefix> <control-file>

The real loader receives an ELF over TCP and runs it. This one receives the
bytes (and ignores them), then runs the already-built host helper binary with
NB_TEST_PREFIX=<prefix> and NB_TEST_PORT=<helper-port>, so the real client and
the real helper protocol are exercised end to end. A connection that sends no
bytes (a reachability check) runs nothing, as the real loader runs nothing
without an ELF.

The control file is JSON, re-read before each spawn:
    {"running": a title ID the helper should treat as open,
     "uninstall_fails": true to make the system uninstall refuse,
     "delete_fails": a name: deleting a file whose path has it fails}
"""

import json
import os
import socket
import subprocess
import sys

LOADER_PORT = int(sys.argv[1])
HELPER = sys.argv[2]
HELPER_PORT = sys.argv[3]
PREFIX = sys.argv[4]
CONTROL = sys.argv[5]


def control():
    try:
        with open(CONTROL) as f:
            return json.load(f)
    except Exception:
        return {}


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", LOADER_PORT))
    srv.listen(4)
    print("ready", flush=True)
    while True:
        conn, _ = srv.accept()
        # Drain the payload the client sends, then close our side.
        got = 0
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                got += len(chunk)
        except OSError:
            pass
        conn.close()
        if got == 0:
            continue  # a reachability check, not an ELF: run nothing
        c = control()
        env = dict(os.environ, NB_TEST_PREFIX=PREFIX, NB_TEST_PORT=HELPER_PORT)
        if c.get("running"):
            env["NB_TEST_RUNNING"] = c["running"]
        else:
            env.pop("NB_TEST_RUNNING", None)
        if c.get("delete_fails"):
            env["NB_TEST_DELETE_FAILS"] = c["delete_fails"]
        else:
            env.pop("NB_TEST_DELETE_FAILS", None)
        if c.get("uninstall_fails"):
            env["NB_TEST_UNINSTALL_FAILS"] = "1"
        else:
            env.pop("NB_TEST_UNINSTALL_FAILS", None)
        # Fire and forget, like the real loader: it runs the payload and the
        # payload opens its own port.
        subprocess.Popen([HELPER], env=env)


if __name__ == "__main__":
    main()
