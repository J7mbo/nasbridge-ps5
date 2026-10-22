"""A real NFS NAS for NASBridge's tests: UNFS3's unfsd (a user-space NFSv3
server), run as whoever runs the tests (no root), exporting one folder.

    python3 tests/nfs_server.py <work-dir> <nfs-port> <mount-port> <exported-folder> [<closed-folder>]

Exports <exported-folder> read-only to 127.0.0.1, NFS on <nfs-port> and its
MOUNT service on <mount-port>, over TCP, without a portmapper (the tests tell
NASBridge the MOUNT port). The export is "insecure": the tests can't use a
privileged port (below 1024), as NASBridge may not on a PS5. <closed-folder>,
when given, is exported to another address only, so mounting it from here is
refused. Runs until killed (SIGTERM).
"""

import os
import signal
import subprocess
import sys

WORK, NFS_PORT, MOUNT_PORT, EXPORTED = sys.argv[1:5]
CLOSED = sys.argv[5] if len(sys.argv) > 5 else None

os.makedirs(WORK, exist_ok=True)
exports = os.path.join(WORK, "exports")
with open(exports, "w") as f:
    f.write(f"{EXPORTED} 127.0.0.1(ro,insecure)\n")
    if CLOSED:
        f.write(f"{CLOSED} 10.255.255.1(ro,insecure)\n")

# -u: not root; -d: in the foreground; -t: TCP only; -p: no portmapper;
# -s: single user (files are served as whoever runs it).
unfsd = subprocess.Popen(["unfsd", "-u", "-d", "-t", "-p", "-s", "-e", exports,
                          "-n", NFS_PORT, "-m", MOUNT_PORT, "-l", "127.0.0.1"],
                         stdout=subprocess.DEVNULL, start_new_session=True)


def stop(*_):
    try:
        os.killpg(unfsd.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
unfsd.wait()
