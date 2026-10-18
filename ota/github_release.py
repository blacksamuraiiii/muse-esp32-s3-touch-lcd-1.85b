#!/usr/bin/env python3
"""Create or update a GitHub release and upload the firmware assets, without gh.

    ota/github_release.py <tag> <notes> <asset> [<asset> ...]

Used by ota/publish_github.sh when the GitHub CLI is not installed. The token
comes from GH_TOKEN / GITHUB_TOKEN, or from the credential helper git already
uses for this remote, so nothing has to be pasted anywhere.

Assets are replaced, not added: the device fetches
releases/latest/download/latest.json and then the tag's muse-gadget.bin, and a
duplicate name would 422 on upload.
"""

import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def die(msg):
    sys.exit("github_release: " + msg)


def token():
    tok = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if tok:
        return tok.strip()
    try:
        out = subprocess.run(
            ["git", "credential", "fill"],
            input="protocol=https\nhost=github.com\n\n",
            capture_output=True, text=True, check=True,
            env=dict(os.environ, GIT_TERMINAL_PROMPT="0"),
        ).stdout
    except subprocess.CalledProcessError:
        die("no GH_TOKEN and no stored git credential for github.com")
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1].strip()
    die("git credential fill returned no password")


def repo_slug():
    repo = os.environ.get("REPO")
    if repo:
        return repo
    try:
        url = subprocess.run(
            ["git", "config", "--get", "remote.origin.url"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except subprocess.CalledProcessError:
        die("set REPO=owner/name (no git remote found)")
    for pre in ("git@github.com:", "https://github.com/"):
        if url.startswith(pre):
            url = url[len(pre):]
    return url[:-4] if url.endswith(".git") else url


def call(method, url, tok, data=None, raw=None, ctype="application/json"):
    body = raw if raw is not None else (
        json.dumps(data).encode() if data is not None else None
    )
    req = urllib.request.Request(url, data=body, method=method, headers={
        "Authorization": "Bearer " + tok,
        "Accept": "application/vnd.github+json",
        "Content-Type": ctype,
        "User-Agent": "muse-publish",
    })
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            text = r.read()
            return r.status, (json.loads(text) if text else None)
    except urllib.error.HTTPError as e:
        detail = e.read()[:400].decode("utf-8", "replace")
        return e.code, detail


def main():
    if len(sys.argv) < 4:
        die(__doc__)
    tag, notes, assets = sys.argv[1], sys.argv[2], sys.argv[3:]
    tok = token()
    repo = repo_slug()
    title = "Muse 1.85B " + tag.lstrip("v")

    status, body = call("POST", "%s/repos/%s/releases" % (API, repo), tok,
                        data={"tag_name": tag, "name": title, "body": notes})
    if status == 201:
        rel = body
        print("created release %s" % tag)
    elif status == 422:
        status, rel = call("GET", "%s/repos/%s/releases/tags/%s" % (API, repo, tag), tok)
        if status != 200:
            die("release %s exists but could not be read: %s" % (tag, rel))
        status, _ = call("PATCH", "%s/repos/%s/releases/%d" % (API, repo, rel["id"]),
                         tok, data={"name": title, "body": notes})
        print("updated release %s" % tag)
    else:
        die("creating the release failed (%s): %s" % (status, body))

    # Replace any asset of the same name: uploads are keyed by name.
    status, current = call("GET", "%s/repos/%s/releases/%d/assets" % (API, repo, rel["id"]), tok)
    existing = {a["name"]: a["id"] for a in (current or [])}
    for path in assets:
        name = os.path.basename(path)
        if name in existing:
            call("DELETE", "%s/repos/%s/releases/assets/%d" % (API, repo, existing[name]), tok)
        with open(path, "rb") as f:
            raw = f.read()
        status, body = call(
            "POST",
            "%s/repos/%s/releases/%d/assets?name=%s" % (UPLOADS, repo, rel["id"], name),
            tok, raw=raw, ctype="application/octet-stream",
        )
        if status not in (200, 201):
            die("uploading %s failed (%s): %s" % (name, status, body))
        print("uploaded %s (%d bytes)" % (name, len(raw)))
    print("release: https://github.com/%s/releases/tag/%s" % (repo, tag))


if __name__ == "__main__":
    main()
