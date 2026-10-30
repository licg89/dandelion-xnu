#!/usr/bin/env python3
"""Wait for a build-xnu run and fetch its mach_kernel artifact.

Why this exists next to fetch_kernel.ps1: that script drives the GitHub API
through PowerShell's Invoke-WebRequest, which hangs indefinitely on some
networks/proxies (even with -NoWait).  This one uses curl.exe for every
request and git-credential-manager for the token, and prints a status line
every 20 s so a stalled network is visible instead of silent.

    py -3.13 tools/ci/waitfetch.py                 # latest build-xnu run
    py -3.13 tools/ci/waitfetch.py 68cdb45         # first run whose sha starts with this
    py -3.13 tools/ci/waitfetch.py 68cdb45 --no-wait

Writes Resources/UniXNU/mach_kernel and prints its size.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import zipfile

REPO = "licg89/dandelion-xnu"


def token_from_git():
    cred = subprocess.run(["git", "credential", "fill"],
                          input="protocol=https\nhost=github.com\n\n",
                          capture_output=True, text=True)
    for line in cred.stdout.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    sys.exit("no github token from git credential fill")


def curl(url, token=None, max_time=120, binary=False):
    cmd = ["curl.exe", "-sSL", "--max-time", str(max_time)]
    if token:
        cmd += ["-H", "Authorization: Bearer " + token,
                "-H", "Accept: application/vnd.github+json"]
    cmd.append(url)
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.exit("curl rc=%d %s" % (r.returncode, r.stderr.decode("utf-8", "replace")[:200]))
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sha", nargs="?", help="commit prefix; default: latest build-xnu run")
    ap.add_argument("--no-wait", action="store_true", help="fail instead of waiting")
    ap.add_argument("--timeout-minutes", type=int, default=60)
    args = ap.parse_args()

    token = token_from_git()
    api = "https://api.github.com/repos/%s" % REPO
    deadline = time.time() + args.timeout_minutes * 60

    run = None
    while True:
        # Authenticate this call too: the unauthenticated rate limit is 60/hour
        # per IP and this loop runs every 20 s, so it used to end in a JSON
        # KeyError ('workflow_runs') the moment the limit was hit.
        runs = json.loads(curl(api + "/actions/runs?per_page=10", token))["workflow_runs"]
        if args.sha:
            run = next((r for r in runs
                        if r["name"] == "build-xnu" and r["head_sha"].startswith(args.sha)), None)
        else:
            run = next((r for r in runs if r["name"] == "build-xnu"), None)
        stamp = time.strftime("%H:%M:%S")
        if not run:
            print("%s  no build-xnu run for %s yet" % (stamp, args.sha or "HEAD"), flush=True)
        else:
            print("%s  run %s  %s  %s" % (stamp, run["id"], run["status"], run["conclusion"]),
                  flush=True)
            if run["status"] == "completed":
                break
        if args.no_wait:
            sys.exit("not finished yet")
        if time.time() > deadline:
            sys.exit("timed out waiting")
        time.sleep(20)

    if run["conclusion"] != "success":
        jobs = json.loads(curl(api + "/actions/runs/%s/jobs" % run["id"], token))["jobs"]
        for job in jobs:
            print("job %s: %s/%s" % (job["name"], job["status"], job["conclusion"]))
            for step in job["steps"]:
                print("   %-40s %s/%s" % (step["name"], step["status"], step["conclusion"]))
        sys.exit("run concluded " + str(run["conclusion"]))

    arts = json.loads(curl(api + "/actions/runs/%s/artifacts" % run["id"], token))["artifacts"]
    art = next((a for a in arts if a["name"].startswith("mach_kernel")), None)
    if not art:
        sys.exit("run succeeded but carries no mach_kernel artifact")
    print("artifact %s (%d bytes)" % (art["name"], art["size_in_bytes"]), flush=True)

    tmp = os.environ.get("TEMP", os.path.join(os.path.expanduser("~"), "tmp"))
    os.makedirs(tmp, exist_ok=True)
    zip_path = os.path.join(tmp, "mach_kernel.zip")
    open(zip_path, "wb").write(curl(
        "%s/actions/artifacts/%s/zip" % (api, art["id"]), token, 900, binary=True))
    print("zip %d bytes" % os.path.getsize(zip_path), flush=True)

    dest = os.path.join(tmp, "mach_kernel_art")
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(dest)

    kernel = None
    for root, _dirs, files in os.walk(dest):
        if "mach_kernel" in files:
            kernel = os.path.join(root, "mach_kernel")
            break
    if not kernel:
        sys.exit("no mach_kernel in artifact")

    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(repo_root, "Resources", "UniXNU", "mach_kernel")
    with open(kernel, "rb") as src, open(out, "wb") as dst:
        dst.write(src.read())
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    print("next: py tools/mk_unixnu_esp.py, then fastboot flash flashinfo out\\unixnu-esp.img")


if __name__ == "__main__":
    main()
