"""End-to-end tests for the NASBridge installer: a host build of the installer
plus tools/install.py, run with `make test` (inside the toolchain container)."""
import hashlib, os, shutil, socket, struct, subprocess, sys, tempfile, time, filecmp

INST = "/src/build/host/installer"
CLIENT = "/src/tools/install.py"
SELFTEST = "/src/build/ps5/selftest/NASBSELFTEST"
PORT = 9245
results = []


def start(prefix, running=None):
    env = dict(os.environ, NB_TEST_PREFIX=prefix, NB_TEST_PORT=str(PORT))
    if running:
        env["NB_TEST_RUNNING"] = running  # the host build treats this title as open
    p = subprocess.Popen([INST], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.3)
    return p


def client(folder, name=None, root="selftest"):
    args = [sys.executable, CLIENT, "--host", "127.0.0.1", "--installer-port", str(PORT), "--root", root, folder]
    if name: args[-1:-1] = ["--name", name]
    return subprocess.run(args, capture_output=True, text=True)


def raw(frames):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    s.sendall(frames)
    try:
        s.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    reply = b""
    while True:
        try:
            d = s.recv(1024)
        except OSError:
            break
        if not d: break
        reply += d
    s.close()
    return reply.decode().strip()


def header(name, count, root=1):
    n = name.encode()
    return b"NBI1" + struct.pack("<BH", root, len(n)) + n + struct.pack("<I", count)


def filerec(path, data, digest=None):
    p = path.encode()
    return struct.pack("<H", len(p)) + p + struct.pack("<Q", len(data)) + (digest or hashlib.sha256(data).digest()) + data


def check(name, cond, detail=""):
    results.append((name, cond))
    print(("PASS " if cond else "FAIL ") + name + (f"  [{detail}]" if detail and not cond else ""))


def tree_equal(a, b):
    c = filecmp.dircmp(a, b)
    if c.left_only or c.right_only or c.diff_files or c.funny_files: return False
    return all(tree_equal(os.path.join(a, d), os.path.join(b, d)) for d in c.common_dirs)


def leftovers(prefix):
    st = os.path.join(prefix, "data/nasbridge/staging")
    return os.listdir(st) if os.path.isdir(st) else []


prefix = tempfile.mkdtemp()
dest = os.path.join(prefix, "data/nasbridge/selftest/NASBSELFTEST")

# 1. happy path with the real self-test folder
p = start(prefix); r = client(SELFTEST); p.wait(5)
check("install self-test folder", r.returncode == 0 and tree_equal(SELFTEST, dest), r.stdout + r.stderr)
def modes_ok(path):
    for dirpath, dirs, files in os.walk(path):
        for n in [dirpath] + [os.path.join(dirpath, f) for f in files]:
            if os.stat(n).st_mode & 0o777 != 0o777:
                return False
    return True
check("installed files and folders are 0777", modes_ok(dest))
check("installer exits 0 and notifies", p.returncode == 0 and "NASBridge installed" in p.stdout.read())
check("no staging leftovers", leftovers(prefix) == [], str(leftovers(prefix)))
check("empty staging folder removed", not os.path.exists(os.path.join(prefix, "data/nasbridge/staging")))

# 2. reinstall replaces the old version completely
alt = tempfile.mkdtemp(); os.makedirs(os.path.join(alt, "x")); open(os.path.join(alt, "x/new.txt"), "w").write("v2")
p = start(prefix); r = client(alt, name="NASBSELFTEST"); p.wait(5)
check("reinstall replaces folder", r.returncode == 0 and tree_equal(alt, dest), r.stdout + r.stderr)
check("no .old or staging leftovers after replace", leftovers(prefix) == [], str(leftovers(prefix)))

# 3. bad checksum: refused, existing install untouched
p = start(prefix)
reply = raw(header("NASBSELFTEST", 1) + filerec("a.txt", b"hello", digest=b"\0" * 32) + b"END1"); p.wait(5)
check("checksum mismatch refused", reply.startswith("ERR checksum mismatch"), reply)
check("existing install untouched after failure", tree_equal(alt, dest))
check("staging cleaned after failure", leftovers(prefix) == [], str(leftovers(prefix)))

# 4. path escapes are refused
for bad in ["../evil", "a/../../evil", "/etc/passwd", "a//b", "./a", "a\\b", "a/..", "ok/\x00x"]:
    p = start(prefix)
    reply = raw(header("NASBSELFTEST", 1) + filerec(bad, b"x") + b"END1"); p.wait(5)
    check(f"unsafe path refused: {bad!r}", reply.startswith("ERR refused unsafe path") or reply.startswith("ERR bad path"), reply)
check("nothing written outside the prefix", not os.path.exists(os.path.join(prefix, "evil")) and not os.path.exists(os.path.join(os.path.dirname(prefix), "evil")))

# 5. bad folder names and destinations
for bad in ["../x", "a/b", "", "x" * 33, "sp ace"]:
    p = start(prefix)
    reply = raw(header(bad, 1) + filerec("a", b"x") + b"END1") if bad else raw(b"NBI1" + struct.pack("<BH", 1, 0)); p.wait(5)
    check(f"bad folder name refused: {bad[:10]!r}", reply.startswith("ERR"), reply)
p = start(prefix); reply = raw(header("X", 1, root=7)); p.wait(5)
check("unknown destination refused", reply.startswith("ERR unknown destination"), reply)
p = start(prefix); reply = raw(b"GET / HTTP/1.1\r\n\r\n"); p.wait(5)
check("non-installer traffic refused", reply.startswith("ERR not a NASBridge"), reply)

# 6. client disconnects mid-file: refused, cleaned up, old install kept
p = start(prefix)
s = socket.create_connection(("127.0.0.1", PORT)); data = b"y" * 100000
s.sendall(header("NASBSELFTEST", 1) + struct.pack("<H", 1) + b"a" + struct.pack("<Q", len(data)) + hashlib.sha256(data).digest() + data[:5000]); s.close()
p.wait(10)
check("truncated stream refused, old install kept", p.returncode == 1 and tree_equal(alt, dest))
check("staging cleaned after truncation", leftovers(prefix) == [], str(leftovers(prefix)))

# 7. stalled client hits the receive timeout (3 s in the test build)
p = start(prefix); s = socket.create_connection(("127.0.0.1", PORT)); s.sendall(b"NBI1"); t = time.time(); p.wait(15); s.close()
check("stalled client times out", p.returncode == 1 and 2 <= time.time() - t < 10, f"{time.time()-t:.1f}s")

# 8. nobody connects: installer gives up and exits (3 s in the test build)
t = time.time(); p = start(prefix); p.wait(15); out = p.stdout.read()
check("no connection: exits on its own", p.returncode == 1 and "nothing connected" in out and time.time() - t < 10)

# 9. one connection only: port is closed after the first client
p = start(prefix)
s1 = socket.create_connection(("127.0.0.1", PORT)); time.sleep(0.3)
try:
    s2 = socket.create_connection(("127.0.0.1", PORT), timeout=2); second = True; s2.close()
except OSError:
    second = False
s1.close(); p.wait(10)
check("second connection is rejected", not second)

# 10. homebrew root goes to /data/homebrew
p = start(prefix); r = client(SELFTEST, root="homebrew"); p.wait(5)
check("homebrew destination", r.returncode == 0 and tree_equal(SELFTEST, os.path.join(prefix, "data/homebrew/NASBSELFTEST")), r.stderr)

# 11. a title that is open on the PS5 is never replaced
hb = os.path.join(prefix, "data/homebrew/NASBSELFTEST")
before = tempfile.mkdtemp(); shutil.rmtree(before); shutil.copytree(hb, before)
p = start(prefix, running="NASBSELFTEST"); r = client(alt, name="NASBSELFTEST", root="homebrew"); p.wait(15)
check("open title refused with a clear reason", r.returncode == 1 and "is open on the PS5" in r.stderr, r.stdout + r.stderr)
check("open title's files untouched", tree_equal(before, hb))
check("no staging leftovers after refusal", leftovers(prefix) == [], str(leftovers(prefix)))
p = start(prefix, running="SOMEOTHER1"); r = client(alt, name="NASBSELFTEST", root="homebrew"); p.wait(5)
check("another open title doesn't block the install", r.returncode == 0 and tree_equal(alt, hb), r.stderr)
p = start(prefix, running="NASBSELFTEST"); r = client(SELFTEST); p.wait(5)
check("self-test folder isn't checked for a running title", r.returncode == 0, r.stderr)
shutil.rmtree(before)
named = os.path.join(prefix, "data/homebrew/TESTAPP01/sce_sys"); os.makedirs(named)
open(os.path.join(named, "param.json"), "w").write('{\n  "titleId": "TESTAPP01",\n  "titleName": "Test App"\n}\n')
p = start(prefix, running="TESTAPP01"); r = client(alt, name="TESTAPP01", root="homebrew"); p.wait(15)
check("refusal names the app, not its title ID", "Test App is open on the PS5" in r.stderr, r.stderr)

# 12. a reinstall refreshes the home screen's copies of the pictures
meta = os.path.join(prefix, "user/appmeta/PICTEST01"); os.makedirs(meta)
open(os.path.join(meta, "icon0.png"), "wb").write(b"old icon")
open(os.path.join(meta, "pic0.png"), "wb").write(b"old pic0 png")
open(os.path.join(meta, "param.json"), "wb").write(b"untouched")
pics = tempfile.mkdtemp(); os.makedirs(os.path.join(pics, "sce_sys"))
for n, d in [("icon0.png", b"new icon"), ("pic0.dds", b"new dds"), ("pic1.dds", b"new dds 1"), ("pic0.png", b"new pic0 png"), ("pic1.png", b"new pic1 png")]:
    open(os.path.join(pics, "sce_sys", n), "wb").write(d)
p = start(prefix); r = client(pics, name="PICTEST01", root="homebrew"); p.wait(5)
got = {n: open(os.path.join(meta, n), "rb").read() for n in os.listdir(meta)}
check("home-screen icon and backgrounds refreshed", r.returncode == 0 and got.get("icon0.png") == b"new icon" and got.get("pic0.dds") == b"new dds" and got.get("pic1.dds") == b"new dds 1" and got.get("pic0.png") == b"new pic0 png", str(got))
check("only pictures that were there are replaced (pic1.png not added), param.json untouched", "pic1.png" not in got and got.get("param.json") == b"untouched", str(sorted(got)))
check("installer doesn't mention the pictures", "Updated the home screen" not in r.stdout, r.stdout)
p = start(prefix); r = client(pics, name="NOMETA001", root="homebrew"); p.wait(5)
check("no appmeta folder: nothing created", r.returncode == 0 and not os.path.exists(os.path.join(prefix, "user/appmeta/NOMETA001")))
shutil.rmtree(pics)

# 13. the real app folder, if built, installs intact and launchable (0777)
apps = sorted(d for d in os.listdir("/src/out/app")) if os.path.isdir("/src/out/app") else []
if apps:
    app = os.path.join("/src/out/app", apps[0]); installed = os.path.join(prefix, "data/homebrew", apps[0])
    p = start(prefix); r = client(app, root="homebrew"); p.wait(5)
    check(f"app folder {apps[0]} installs intact", r.returncode == 0 and tree_equal(app, installed), r.stderr)
    check(f"app folder {apps[0]} is 0777 throughout", modes_ok(installed))

# 14. make uninstall: the folder goes, then the home-screen entry; never while open
def uninstall(name):
    args = [sys.executable, CLIENT, "--host", "127.0.0.1", "--installer-port", str(PORT), "--uninstall", name]
    return subprocess.run(args, capture_output=True, text=True)
gone = os.path.join(prefix, "data/homebrew/UNINST001")
p = start(prefix); client(alt, name="UNINST001", root="homebrew"); p.wait(5)
p = start(prefix, running="UNINST001"); r = uninstall("UNINST001"); p.wait(15)
check("uninstall refused while the app is open", r.returncode == 1 and "is open on the PS5" in r.stderr and tree_equal(alt, gone), r.stdout + r.stderr)
p = start(prefix); r = uninstall("UNINST001"); p.wait(5)
done = open(os.path.join(prefix, "uninstalled.txt")).read() if os.path.exists(os.path.join(prefix, "uninstalled.txt")) else ""
check("uninstall deletes the folder and removes it from the home screen", r.returncode == 0 and not os.path.exists(gone) and "UNINST001" in done, r.stdout + r.stderr + done)
check("uninstall notifies", "NASBridge uninstalled" in p.stdout.read())
p = start(prefix); r = uninstall("../evil"); p.wait(5)
check("uninstall refuses a bad name", r.returncode == 1 and "refused folder name" in r.stderr, r.stderr)
check("other apps untouched by uninstall", tree_equal(alt, os.path.join(prefix, "data/homebrew/NASBSELFTEST")))

shutil.rmtree(prefix); shutil.rmtree(alt)
failed = [n for n, ok in results if not ok]
print(f"\n{len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
