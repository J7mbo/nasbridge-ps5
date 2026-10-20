#!/usr/bin/env python3
"""How fast the NAS serves small files, 1, 4, 8 and 16 at once (a diagnostic).

Copying a folder game of tens of thousands of small files runs well below
the network's speed. This fetches some of that game's small files from a
computer, with keep-alive connections as NASBridge's copy helper uses, to see
whether the NAS itself is the limit. Each run takes its own files (from the
end of the list, which a copy in progress hasn't reached), so the NAS's cache
doesn't flatter a later run. Pause any copy while it runs.

    python3 tools/nas_bench.py --listing /tmp/deep2.xml --host 192.168.1.108 \
        --port 6060 --user ps5 --password ps5

The listing is the saved answer to a "Depth: infinity" PROPFIND of the game's
folder (see the curl command it was made with).
"""

import argparse
import base64
import http.client
import re
import threading
import time
import urllib.parse

SMALL = 16 * 1024 * 1024  # NASBridge's batches take files under this


def small_files(listing):
    """(href, size) of every file under SMALL in a PROPFIND answer, in order."""
    text = open(listing, encoding="utf-8", errors="replace").read()
    files = []
    for block in re.findall(r"<[A-Za-z]*:?response>(.*?)</[A-Za-z]*:?response>", text, re.S):
        href = re.search(r"<[A-Za-z]*:?href>([^<]*)<", block)
        size = re.search(r"getcontentlength>(\d+)<", block)
        if href and size and int(size.group(1)) < SMALL:
            path = href.group(1)
            if path.startswith("http"):
                path = urllib.parse.urlparse(path).path
            files.append((path, int(size.group(1))))
    return files


def fetch_all(args, files, workers):
    """Fetches `files` with `workers` keep-alive connections; (seconds, bytes, errors)."""
    auth = "Basic " + base64.b64encode(f"{args.user}:{args.password}".encode()).decode()
    lock = threading.Lock()
    queue = list(files)
    totals = {"bytes": 0, "errors": 0}

    def worker():
        conn = http.client.HTTPConnection(args.host, args.port, timeout=30)
        while True:
            with lock:
                if not queue:
                    break
                path, _ = queue.pop(0)
            try:
                conn.request("GET", path, headers={"Authorization": auth,
                                                   "Connection": "keep-alive"})
                reply = conn.getresponse()
                body = reply.read()
                with lock:
                    if reply.status == 200:
                        totals["bytes"] += len(body)
                    else:
                        totals["errors"] += 1
            except Exception:
                with lock:
                    totals["errors"] += 1
                conn.close()
                conn = http.client.HTTPConnection(args.host, args.port, timeout=30)
        conn.close()

    threads = [threading.Thread(target=worker) for _ in range(workers)]
    start = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return time.monotonic() - start, totals["bytes"], totals["errors"]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--listing", default="/tmp/deep2.xml")
    ap.add_argument("--host", default="192.168.1.108")
    ap.add_argument("--port", type=int, default=6060)
    ap.add_argument("--user", default="ps5")
    ap.add_argument("--password", default="ps5")
    ap.add_argument("--files", type=int, default=300, help="files per run")
    args = ap.parse_args()

    files = small_files(args.listing)
    if not files:
        raise SystemExit(f"no files found in {args.listing}")
    runs = [1, 4, 8, 16]
    need = args.files * len(runs)
    tail = files[-need:] if len(files) >= need else files
    per = max(1, len(tail) // len(runs))
    avg = sum(size for _, size in tail) / len(tail)
    print(f"{len(files):,} small files listed; {per} per run from the end, "
          f"{avg / 1024:,.0f} KiB on average")
    for i, workers in enumerate(runs):
        chunk = tail[i * per:(i + 1) * per]
        seconds, got, errors = fetch_all(args, chunk, workers)
        note = f"  ({errors} failed)" if errors else ""
        print(f"{workers:2d} at once: {len(chunk) / seconds:6.1f} files/s, "
              f"{got / seconds / 1e6:6.1f} MB/s{note}")


if __name__ == "__main__":
    main()
