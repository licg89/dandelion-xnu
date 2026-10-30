#!/usr/bin/env python3
"""Print the interesting lines of the latest build-xnu run's log.

Written because every attempt to do this through PowerShell inline quoting failed:
nested quotes, here-strings and < redirections all have to survive a layer of pwsh
before bash sees them.  A file has no such problem.

    py -3.13 tools/ci/ci_log.py            # latest run
    py -3.13 tools/ci/ci_log.py --all      # whole log, filtered to errors/warnings
"""
import argparse
import io
import json
import subprocess
import sys
import urllib.request
import zipfile

REPO = "licg89/dandelion-xnu"


def token():
    out = subprocess.run(["git", "credential", "fill"], cwd=".",
                         input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    sys.exit("no github token from git credential fill")


def api(path, tok, raw=False):
    request = urllib.request.Request(
        "https://api.github.com" + path,
        headers={"Authorization": "token " + tok, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=60) as response:
        payload = response.read()
    return payload if raw else json.loads(payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--run", type=int, default=None)
    args = ap.parse_args()

    tok = token()
    if args.run:
        run_id = args.run
    else:
        runs = api("/repos/%s/actions/runs?per_page=10" % REPO, tok)["workflow_runs"]
        runs = [r for r in runs if r["name"] == "build-xnu"]
        if not runs:
            sys.exit("no build-xnu runs")
        run = runs[0]
        run_id = run["id"]
        print("run %d  %s/%s  sha=%s" % (run_id, run["status"], run["conclusion"],
                                         run["head_sha"][:8]))

    jobs = api("/repos/%s/actions/runs/%d/jobs" % (REPO, run_id), tok)["jobs"]
    for job in jobs:
        print("  job %d %s: %s/%s" % (job["id"], job["name"], job["status"], job["conclusion"]))
        if job["conclusion"] != "failure":
            continue
        blob = api("/repos/%s/actions/jobs/%d/logs" % (REPO, job["id"]), tok, raw=True)
        try:
            archive = zipfile.ZipFile(io.BytesIO(blob))
            text = "".join(archive.read(name).decode("utf-8", "replace")
                           for name in archive.namelist())
        except zipfile.BadZipFile:
            text = blob.decode("utf-8", "replace")

        print("  --- interesting lines ---")
        shown = 0
        for line in text.splitlines():
            low = line.lower()
            interesting = ("error:" in low or "error " in low and "0 error" not in low
                           or "failed" in low or "does not apply" in low)
            if args.all or interesting:
                print("   " + line.strip()[:240])
                shown += 1
                if shown > 60 and not args.all:
                    print("   ... (truncated)")
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
