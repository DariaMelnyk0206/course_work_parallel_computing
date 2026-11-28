#!/usr/bin/env python3
"""Downloads the PG-19 subset (train/<id>.txt, 10 <= id <= 30000) from the
public deepmind-gutenberg bucket over HTTPS. Uses only the standard library.
"""

import argparse
import concurrent.futures
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BUCKET = "deepmind-gutenberg"
LIST_URL = "https://storage.googleapis.com/storage/v1/b/%s/o" % BUCKET
FILE_URL = "https://storage.googleapis.com/%s/train/%%d.txt" % BUCKET
NAME_RE = re.compile(r"^train/(\d+)\.txt$")
MIN_ID = 10
MAX_ID = 30000
RETRIES = 4
TIMEOUT_S = 60
CHUNK = 1 << 20


def http_open(url):
    return urllib.request.urlopen(url, timeout=TIMEOUT_S)


def list_files():
    """Returns [(id, size)] sorted by numeric id, following nextPageToken."""
    files = {}
    token = None
    while True:
        params = {"prefix": "train/", "fields": "items(name,size),nextPageToken"}
        if token:
            params["pageToken"] = token
        url = LIST_URL + "?" + urllib.parse.urlencode(params)
        for attempt in range(RETRIES):
            try:
                with http_open(url) as resp:
                    page = json.load(resp)
                break
            except (urllib.error.URLError, OSError, ValueError):
                if attempt == RETRIES - 1:
                    raise
                time.sleep(1 + attempt)
        for item in page.get("items", []):
            m = NAME_RE.match(item["name"])
            if m and MIN_ID <= int(m.group(1)) <= MAX_ID:
                files[int(m.group(1))] = int(item["size"])
        token = page.get("nextPageToken")
        if not token:
            break
    return sorted(files.items())


def download(file_id, size, out_dir):
    """Returns 'downloaded', 'skipped' or raises on failure."""
    path = os.path.join(out_dir, "%d.txt" % file_id)
    if os.path.isfile(path) and os.path.getsize(path) == size:
        return "skipped"
    part = path + ".part"
    last_err = None
    for attempt in range(RETRIES):
        try:
            written = 0
            with http_open(FILE_URL % file_id) as resp, open(part, "wb") as f:
                while True:
                    chunk = resp.read(CHUNK)
                    if not chunk:
                        break
                    f.write(chunk)
                    written += len(chunk)
            if written != size:
                raise OSError("size mismatch: got %d, expected %d" % (written, size))
            os.replace(part, path)
            return "downloaded"
        except (urllib.error.URLError, OSError) as e:
            last_err = e
            time.sleep(1 + attempt)
    if os.path.exists(part):
        os.remove(part)
    raise last_err


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="./dataset", help="output directory")
    ap.add_argument("--limit", type=int, default=None, metavar="K",
                    help="download only the first K files by id")
    ap.add_argument("--workers", type=int, default=16,
                    help="parallel downloads (default 16)")
    args = ap.parse_args()
    if args.limit is not None and args.limit < 1:
        ap.error("--limit must be >= 1")
    if args.workers < 1:
        ap.error("--workers must be >= 1")

    os.makedirs(args.out, exist_ok=True)
    try:
        files = list_files()
    except (urllib.error.URLError, OSError, ValueError) as e:
        print("Failed to list bucket: %s" % e, file=sys.stderr)
        return 1
    if args.limit is not None:
        files = files[:args.limit]
    total_bytes = sum(s for _, s in files)
    print("Files: %d (%.2f GB)" % (len(files), total_bytes / 1e9))

    counts = {"downloaded": 0, "skipped": 0, "errors": 0}
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as ex:
        futures = {ex.submit(download, i, s, args.out): i for i, s in files}
        for fut in concurrent.futures.as_completed(futures):
            done += 1
            try:
                counts[fut.result()] += 1
            except Exception as e:  # noqa: BLE001
                counts["errors"] += 1
                print("Error on %d.txt: %s" % (futures[fut], e), file=sys.stderr)
            if done % 100 == 0 or done == len(files):
                print("Progress: %d/%d" % (done, len(files)), flush=True)

    print("Summary: downloaded=%d skipped=%d errors=%d" %
          (counts["downloaded"], counts["skipped"], counts["errors"]))
    return 1 if counts["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
