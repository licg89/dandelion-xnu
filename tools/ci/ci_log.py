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
import re
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
    url = "https://api.github.com" + path
    request = urllib.request.Request(
        url,
        headers={"Authorization": "token " + tok, "Accept": "application/vnd.github+json"})

    if not raw:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read())

    # The logs endpoint answers 302 to blob storage, and blob storage rejects the
    # Authorization header ("Server failed to authenticate the request").  So follow
    # the redirect by hand and fetch the target with no headers at all; the URL is
    # pre-signed and carries its own credentials.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    opener = urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        location = error.headers.get("Location")
        if not location:
            raise
    with urllib.request.urlopen(location, timeout=180) as response:
        return response.read()


def interesting(line):
    """Classify a log line.  Returns 0 for noise, 1 for 'maybe', 2 for 'this is it'.

    The first version matched a loose 'error' substring and stopped after 60
    hits, which on a failing XNU build are all consumed by warnings text long
    before the compiler's actual complaint.  Three pushes were spent on a patch
    that failed with `pe_fiq.c:301:1: error: use of undeclared identifier`.  So
    the strict patterns come first and are never truncated.
    """
    low = line.lower()
    if re.search(r":\s*(fatal )?error:", low):
        return 2
    if "does not apply" in low or "patch failed" in low:
        return 2
    if "##[error]" in low or "*** [" in line or "error 1" in low:
        return 2
    if "undefined symbol" in low or "undeclared" in low or "no such file" in low:
        return 2
    if re.search(r"\bwarning:", low):
        return 1
    if "failed" in low:
        return 1
    return 0


def main():
    # Clang's message for the failure this tool exists to report contains
    # U+FEFF, which a Windows console (code page 936) cannot encode: without
    # this the tool dies while printing the very line that explains the build
    # failure.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--run", type=int, default=None)
    ap.add_argument("--job", type=int, default=None,
                    help="job id (default: every job of the run)")
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
        if args.job and job["id"] != args.job:
            continue
        print("  job %d %s: %s/%s" % (job["id"], job["name"], job["status"], job["conclusion"]))
        if job["conclusion"] != "failure" and not args.all:
            continue
        blob = api("/repos/%s/actions/jobs/%d/logs" % (REPO, job["id"]), tok, raw=True)
        try:
            archive = zipfile.ZipFile(io.BytesIO(blob))
            text = "".join(archive.read(name).decode("utf-8", "replace")
                           for name in archive.namelist())
        except zipfile.BadZipFile:
            text = blob.decode("utf-8", "replace")

        strict, loose = [], []
        seen = set()
        for line in text.splitlines():
            # strip the runner's timestamp prefix
            stripped = line.split("Z ", 1)[-1].strip()
            if not stripped or stripped in seen:
                continue
            rank = interesting(stripped)
            if rank == 2:
                seen.add(stripped)
                strict.append(stripped)
            elif rank == 1 and args.all:
                seen.add(stripped)
                loose.append(stripped)

        print("  --- compiler errors / failures (all of them) ---")
        for line in strict or ["(none found: see %s)" % job["id"]]:
            print("   " + line[:400])
        if loose:
            print("  --- warnings and other 'failed' lines (first 40) ---")
            for line in loose[:40]:
                print("   " + line[:300])
    return 0


if __name__ == "__main__":
    sys.exit(main())
