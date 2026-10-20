"""A stand-in NAS (WebDAV: PROPFIND and ranged GET) and a stand-in
ShadowMountPlus API, for testing NASBridge's copy and delete on a computer.

    python3 tests/fake_nas.py <games-folder> <nas-port> <smp-port> <control-file>

The control file is JSON, re-read on every request, so a test can change the
servers' behaviour while they run:
    {"throttle": bytes per second for GET (0: as fast as possible),
     "drop_after": end a GET after this many bytes, once (0: never),
     "ignore_range": answer ranged GETs with the whole file,
     "stray_after": answer the ranged GET after this many with the whole file, once,
     "busy": answer this many ranged GETs with 503 (counted down),
     "games": the list /api/v1/games returns,
     "delete_fails": the delete job ends "failed",
     "delete_refused": the delete job fails with this result_error_reason ("game_active"),
                       as ShadowMountPlus does while NASBridge counts as a running game,
     "scan_deferred": the scan reply's scan_deferred_reason ("game_active"),
     "capabilities": what /api/v1/version lists (default: no requester support),
     "delete_request_refused": refuse /api/v1/games/delete at once with this
                               error_reason ("game_active"),
     "uninstall_refused": refuse /api/v1/games/uninstall with this error_reason,
     "uninstall_busy": refuse the first this-many uninstalls as scanner_busy}
Every ShadowMountPlus call is appended to <control-file>.log.
"""

import json
import os
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.sax.saxutils import escape

ROOT = os.path.abspath(sys.argv[1])
NAS_PORT = int(sys.argv[2])
SMP_PORT = int(sys.argv[3])
CONTROL = sys.argv[4]
dropped = {"done": False}
ranged = {"count": 0, "strayed": False, "busy": 0, "busy_set": None}
ranged_lock = threading.Lock()
jobs = {"next": 1, "polls": 0}
gets = {"count": 0}


def control():
    try:
        with open(CONTROL) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def response(path, full):
    href = urllib.parse.quote(path)
    if os.path.isdir(full):
        if not href.endswith("/"):
            href += "/"
        props = "<D:resourcetype><D:collection/></D:resourcetype>"
    else:
        props = ("<D:resourcetype/><D:getcontentlength>%d</D:getcontentlength>"
                 % os.path.getsize(full))
    return ("<D:response><D:href>%s</D:href><D:propstat><D:prop>%s"
            "<D:getlastmodified>Sun, 18 Oct 2026 10:00:00 GMT</D:getlastmodified>"
            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
            % (escape(href), props))


class Nas(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        with open(CONTROL + ".conn.log", "a") as log:  # one line per connection
            log.write("connect\n")

    def log_message(self, *args):
        pass

    def local(self):
        path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
        return path, os.path.join(ROOT, path.lstrip("/"))

    def do_PROPFIND(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length:
            self.rfile.read(length)
        path, full = self.local()
        if not os.path.exists(full):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        depth = self.headers.get("Depth", "1")
        c = control()
        with open(CONTROL + ".nas.log", "a") as log:
            log.write("PROPFIND " + depth + " " + path + "\n")
        if depth == "infinity" and c.get("deep_silent_s"):
            time.sleep(c["deep_silent_s"])  # builds it all first, or never answers
            self.close_connection = True
            return
        if depth == "1" and c.get("folder_delay_ms"):
            time.sleep(c["folder_delay_ms"] / 1000.0)  # a slow disk
        parts = [response(path, full)]
        if os.path.isdir(full):
            if depth == "infinity":
                for here, dirs, files in os.walk(full):
                    rel = os.path.relpath(here, ROOT)
                    for name in sorted(dirs) + sorted(files):
                        p = "/" + os.path.join(rel, name).replace(os.sep, "/")
                        parts.append(response(p, os.path.join(here, name)))
            elif depth == "1":
                for name in sorted(os.listdir(full)):
                    p = path.rstrip("/") + "/" + name
                    parts.append(response(p, os.path.join(full, name)))
        body = ('<?xml version="1.0" encoding="utf-8"?><D:multistatus xmlns:D="DAV:">'
                + "".join(parts) + "</D:multistatus>").encode()
        self.send_response(207)
        self.send_header("Content-Type", "application/xml; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for at in range(0, len(body), 4096):
            piece = body[at:at + 4096]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
        self.wfile.write(b"0\r\n\r\n")

    def do_GET(self):
        path, full = self.local()
        c = control()
        if c.get("drop_get"):
            with ranged_lock:
                if gets.get("set") != c["drop_get"]:  # counted from when it was set
                    gets.update(set=c["drop_get"], count=0)
                gets["count"] += 1
                if gets["count"] == c["drop_get"]:  # this one: the connection just ends
                    self.close_connection = True
                    self.connection.shutdown(2)
                    return
        if not os.path.isfile(full):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        c = control()
        size = os.path.getsize(full)
        start = 0
        rng = self.headers.get("Range")
        stray = False
        if rng:
            with ranged_lock:
                after = c.get("stray_after", 0)
                if after != ranged.get("stray_set"):
                    # Counted from when the option was set.
                    ranged.update(stray_set=after, count=0, strayed=False)
                ranged["count"] += 1
                if after and not ranged["strayed"] and ranged["count"] > after:
                    ranged["strayed"] = True
                    stray = True
                if c.get("busy", 0) != ranged["busy_set"]:
                    ranged["busy_set"] = c.get("busy", 0)
                    ranged["busy"] = ranged["busy_set"]
                busy = ranged["busy"] > 0
                if busy:
                    ranged["busy"] -= 1
            if busy:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        if rng and not c.get("ignore_range") and not stray:
            start = int(rng.split("=")[1].split("-")[0])
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(size - start))
        self.end_headers()
        sent = 0
        with open(full, "rb") as f:
            f.seek(start)
            while True:
                c = control()
                piece = f.read(64 * 1024)
                if not piece:
                    break
                drop = c.get("drop_after", 0)
                if drop and not dropped["done"] and sent + len(piece) > drop:
                    dropped["done"] = True
                    self.wfile.write(piece[: max(0, drop - sent)])
                    self.wfile.flush()
                    self.close_connection = True
                    self.connection.shutdown(2)
                    return
                try:
                    self.wfile.write(piece)
                except OSError:
                    return
                sent += len(piece)
                throttle = c.get("throttle", 0)
                if throttle:
                    time.sleep(len(piece) / throttle)


class Smp(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def answer(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length) or b"{}")
        route = urllib.parse.urlparse(self.path).path
        with open(CONTROL + ".log", "a") as log:
            log.write(route + " " + json.dumps(request, sort_keys=True) + "\n")
        c = control()
        if route == "/api/v1/version":
            return self.answer(200, {"status": 0, "version": "test",
                                     "capabilities": c.get("capabilities", [
                                         "rescan", "uninstall_game", "delete_game_source",
                                         "storage_job_status"])})
        if route == "/api/v1/games":
            games = c.get("games", [])
            return self.answer(200, {"status": 0, "count": len(games), "size_included": False,
                                     "games": games})
        if route == "/api/v1/scan":
            deferred = c.get("scan_deferred", "")
            return self.answer(200, {"status": 0, "scan_queued": True,
                                     "scan_deferred": bool(deferred),
                                     "scan_deferred_reason": deferred})
        if route == "/api/v1/games/delete":
            if c.get("delete_request_refused"):
                return self.answer(409, {"status": 16, "error": "refused",
                                         "error_reason": c["delete_request_refused"]})
            job = jobs["next"]
            jobs["next"] += 1
            jobs["polls"] = 0
            return self.answer(202, {"status": 0, "job_id": job})
        if route == "/api/v1/games/storage/status":
            jobs["polls"] += 1
            refused = c.get("delete_refused", "")
            state = "deleting" if jobs["polls"] < 3 else (
                "failed" if c.get("delete_fails") or refused else "completed")
            return self.answer(200, {"status": 0, "job_id": request.get("job_id", 0),
                                     "operation": "delete", "state": state,
                                     "active": state == "deleting",
                                     # A refusal comes before anything is deleted.
                                     "processed_bytes": 0 if refused else 50 * jobs["polls"],
                                     "total_bytes": 150,
                                     "result_error": ("a game is active or awaiting exit; close "
                                                      "the game and try again") if refused and
                                     state == "failed" else
                                     "disk exploded" if state == "failed" else "",
                                     "result_error_reason": refused if state == "failed" else ""})
        if route == "/api/v1/games/uninstall":
            busy = c.get("uninstall_busy", 0)
            if busy and jobs.setdefault("uninstall_asks", 0) < busy:
                jobs["uninstall_asks"] += 1
                return self.answer(409, {"status": 16, "error": "Device busy",
                                         "error_reason": "scanner_busy"})
            if c.get("uninstall_refused"):
                return self.answer(409, {"status": 16, "error": "Device busy",
                                         "error_reason": c["uninstall_refused"]})
            return self.answer(200, {"status": 0, "scan_queued": False})
        return self.answer(404, {"status": 2, "error": "no such route"})


def serve(server):
    server.serve_forever()


if __name__ == "__main__":
    nas = ThreadingHTTPServer(("127.0.0.1", NAS_PORT), Nas)
    smp = ThreadingHTTPServer(("127.0.0.1", SMP_PORT), Smp)
    threading.Thread(target=serve, args=(smp,), daemon=True).start()
    print("ready", flush=True)
    serve(nas)
