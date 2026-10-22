"""A real SMB NAS for NASBridge's tests: Samba's smbd, run as whoever runs
the tests (no root), sharing one folder.

    python3 tests/smb_server.py <work-dir> <port> <shared-folder>

Shares <shared-folder> as "Games" (read-only) to the user "nbtest" with the
password "secret1", on 127.0.0.1:<port>, until killed. Every message is
signed, as many NASes insist, so the tests go through libsmb2's signing
(and NASBridge's AES, app/third_party/libsmb2/aes_fast.c). Samba wants a Unix
account for each user; the test may run as a user id with no account (the
toolchain container runs as yours), so nss_wrapper (as Samba's own test
suite uses) gives the running user the name "nbtest" for smbd alone.
SIGTERM stops it.
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile

WORK, PORT, SHARED = sys.argv[1], sys.argv[2], sys.argv[3]
USER, PASSWORD = "nbtest", "secret1"
NSS_WRAPPER = "/usr/lib/x86_64-linux-gnu/libnss_wrapper.so"
if not os.path.exists(NSS_WRAPPER):
    NSS_WRAPPER = "/usr/lib/aarch64-linux-gnu/libnss_wrapper.so"

# Samba keeps sockets among its state, and a socket's path must be short
# (about 100 bytes): its state goes in /tmp, its configuration and log here.
RUN = tempfile.mkdtemp(prefix="nbsmb-")
for sub in ("lock", "state", "cache", "private"):
    os.makedirs(os.path.join(RUN, sub), exist_ok=True)
os.makedirs(os.path.join(WORK, "log"), exist_ok=True)

uid, gid = os.getuid(), os.getgid()
passwd = os.path.join(WORK, "passwd")
group = os.path.join(WORK, "group")
with open(passwd, "w") as f:
    f.write(f"{USER}:x:{uid}:{gid}:NASBridge test:{WORK}:/bin/false\n")
    if uid != 0:
        f.write("root:x:0:0:root:/root:/bin/false\n")
with open(group, "w") as f:
    f.write(f"nbtest:x:{gid}:\n")
    if gid != 0:
        f.write("root:x:0:\n")

conf = os.path.join(WORK, "smb.conf")
with open(conf, "w") as f:
    f.write(f"""[global]
  server role = standalone server
  smb ports = {PORT}
  interfaces = 127.0.0.1
  bind interfaces only = yes
  disable netbios = yes
  pid directory = {RUN}
  lock directory = {RUN}/lock
  state directory = {RUN}/state
  cache directory = {RUN}/cache
  private dir = {RUN}/private
  ncalrpc dir = {RUN}/ncalrpc
  log file = {WORK}/log/log.%m
  log level = 1
  passdb backend = tdbsam:{RUN}/private/passdb.tdb
  guest account = nbtest
  server min protocol = SMB2_02
  server signing = mandatory
  load printers = no
  printing = bsd
  printcap name = /dev/null
  disable spoolss = yes
  rpc_server:srvsvc = embedded
  rpc_daemon:spoolssd = disabled
  rpc_server:winreg = embedded
[Games]
  path = {SHARED}
  read only = yes
  guest ok = no
""")

env = dict(os.environ, LD_PRELOAD=NSS_WRAPPER, NSS_WRAPPER_PASSWD=passwd,
           NSS_WRAPPER_GROUP=group)
subprocess.run(["pdbedit", f"--configfile={conf}", "-a", "-t", "-u", USER],
               input=f"{PASSWORD}\n{PASSWORD}\n", text=True, env=env, check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
# smbd in the foreground stops when its input ends, so it gets a pipe that
# stays open; it runs in a session of its own (it signals its whole group
# when it stops), and goes when this does.
smbd = subprocess.Popen(["smbd", "--foreground", "--no-process-group", f"--configfile={conf}",
                         "--debug-stdout", "--debuglevel=0"],
                        env=env, stdin=subprocess.PIPE, start_new_session=True)


def stop(*_):
    try:
        os.killpg(smbd.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
smbd.wait()
shutil.rmtree(RUN, ignore_errors=True)
