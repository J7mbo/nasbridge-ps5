"""A real FTP NAS for NASBridge's tests: pyftpdlib, run as whoever runs the
tests, sharing one folder.

    python3 tests/ftp_server.py <port> <shared-folder> [--list-only] [--per-address N]

Shares <shared-folder> as "/" (read-only) to the user "nbtest" with the
password "secret1", on 127.0.0.1:<port>, until killed. --list-only takes
MLSD (and MLST) away, so folders are listed with LIST, as vsftpd (Unraid's
FTP server) does. --per-address N turns away connections past N from one
address ("421 Too many connections"), as many NAS servers can. SIGTERM stops it.
"""

import logging
import signal
import sys

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import ThreadedFTPServer

PORT, SHARED = int(sys.argv[1]), sys.argv[2]
LIST_ONLY = "--list-only" in sys.argv
PER_ADDRESS = 0
if "--per-address" in sys.argv:
    PER_ADDRESS = int(sys.argv[sys.argv.index("--per-address") + 1])

logging.basicConfig(level=logging.ERROR)  # not each connection turned away

authorizer = DummyAuthorizer()
authorizer.add_user("nbtest", "secret1", SHARED, perm="elr")  # enter, list, read


class Handler(FTPHandler):
    pass


Handler.authorizer = authorizer
Handler.banner = "NASBridge test FTP server"
if LIST_ONLY:
    Handler.proto_cmds = {name: info for name, info in FTPHandler.proto_cmds.items()
                          if name not in ("MLSD", "MLST")}

server = ThreadedFTPServer(("127.0.0.1", PORT), Handler)
server.max_cons_per_ip = PER_ADDRESS


def stop(*_):
    server.close_all()
    sys.exit(0)


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
server.serve_forever()
