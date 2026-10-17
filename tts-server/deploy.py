#!/usr/bin/env python3
"""Deploy tts-server to the LAN host (10.168.2.7) and verify it there.

    MUSE_HOST_PASSWORD='<root password>' python3 deploy.py [host]

Pushes tts_server.py / requirements.txt / Dockerfile to /opt/muse-tts/, writes
the Azure credentials there as .env (mode 600, never into the image or the repo),
builds muse-tts:<TAG> and restarts the container with --env-file so the key
never lands in the container's command line either. Then it asks the host for
/health and synthesises one short line to prove the audio path works.

The password is read from the environment, not argv, so it does not show up in
`ps` or the shell history.
"""

import os
import sys
import time

import paramiko

HOST = sys.argv[1] if len(sys.argv) > 1 else "10.168.2.7"
USER = os.environ.get("MUSE_HOST_USER", "root")
PASSWORD = os.environ.get("MUSE_HOST_PASSWORD", "")
REMOTE_DIR = "/opt/muse-tts"
IMAGE = os.environ.get("TTS_IMAGE_TAG", "muse-tts:1.3")
CONTAINER = "muse-tts"
HERE = os.path.dirname(os.path.abspath(__file__))

UPLOADS = ["tts_server.py", "requirements.txt", "Dockerfile"]


def env_file_text():
    """The .env to place on the host: only what the service needs."""
    wanted = ("AZURE_SPEECH_KEY", "AZURE_SPEECH_REGION", "TTS_VOICE",
              "TTS_FORCE_VOICE", "AZURE_SPEECH_STYLE")
    lines = []
    with open(os.path.join(HERE, ".env")) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key = line.split("=", 1)[0].strip()
            if key in wanted:
                lines.append(line)
    missing = {"AZURE_SPEECH_KEY", "AZURE_SPEECH_REGION"} - {l.split("=", 1)[0] for l in lines}
    if missing:
        sys.exit("local .env is missing: %s" % ", ".join(sorted(missing)))
    return "\n".join(lines) + "\n"


def run(ssh, cmd, timeout=600, quiet=False):
    """Run a command, streaming its output; returns (exit_status, output)."""
    chan = ssh.get_transport().open_session()
    chan.settimeout(timeout)
    chan.exec_command(cmd)
    out = []
    while True:
        if chan.recv_ready():
            chunk = chan.recv(65536).decode("utf-8", "replace")
            out.append(chunk)
            if not quiet:
                sys.stdout.write(chunk)
                sys.stdout.flush()
        elif chan.exit_status_ready():
            break
        else:
            time.sleep(0.2)
    while chan.recv_ready():
        out.append(chan.recv(65536).decode("utf-8", "replace"))
    return chan.recv_exit_status(), "".join(out)


def main():
    if not PASSWORD:
        sys.exit("set MUSE_HOST_PASSWORD first (root password of %s)" % HOST)

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(HOST, username=USER, password=PASSWORD, timeout=15, banner_timeout=15)
    sftp = ssh.open_sftp()

    print("=== upload to %s:%s" % (HOST, REMOTE_DIR))
    run(ssh, "mkdir -p %s" % REMOTE_DIR, quiet=True)
    for name in UPLOADS:
        remote = "%s/%s" % (REMOTE_DIR, name)
        sftp.put(os.path.join(HERE, name), remote)
        print("  %s" % name)

    # The credentials go in a 600 file the container reads with --env-file.
    with sftp.open("%s/.env" % REMOTE_DIR, "w") as f:
        f.write(env_file_text())
    run(ssh, "chmod 600 %s/.env" % REMOTE_DIR, quiet=True)
    print("  .env (mode 600, %d keys)" % env_file_text().count("="))

    print("=== build %s" % IMAGE)
    rc, _ = run(ssh, "cd %s && docker build -t %s . 2>&1 | tail -20" % (REMOTE_DIR, IMAGE))
    if rc != 0:
        sys.exit("docker build failed (%d)" % rc)

    print("=== restart %s" % CONTAINER)
    rc, _ = run(ssh, "docker rm -f %s >/dev/null 2>&1; docker run -d --name %s "
                     "--restart unless-stopped --env-file %s/.env -p 5050:5050 %s"
                % (CONTAINER, CONTAINER, REMOTE_DIR, IMAGE))
    if rc != 0:
        sys.exit("docker run failed (%d)" % rc)

    print("=== verify")
    time.sleep(3)
    rc, out = run(ssh, "curl -sS -m 10 http://127.0.0.1:5050/health", quiet=True)
    print("health: %s" % out.strip())
    if rc != 0 or '"backend"' not in out:
        sys.exit("health check failed")
    rc, out = run(ssh, "curl -sS -m 30 -o /tmp/t.mp3 -w 'http=%{http_code} first_byte=%{time_starttransfer}s "
                       "bytes=%{size_download} backend=%header{X-TTS-Backend}' "
                       "'http://127.0.0.1:5050/tts?text=%E4%BD%A0%E5%A5%BD%EF%BC%8C%E6%88%91%E6%98%AF%E6%85%95"
                       "%E6%80%9D%E6%9D%BF%E5%AD%90%E3%80%82'", quiet=True)
    tts_ok = "http=200" in out
    print("tts   : %s" % out.strip())
    # The host is a bare Alpine image: no ffprobe there, so the MP3 magic in the
    # first bytes is the check. 0xFF sync or ID3, same rule the server applies.
    rc, out = run(ssh, "head -c 4 /tmp/t.mp3 | od -An -tx1", quiet=True)
    head = out.strip()
    print("magic : %s" % head)
    if not tts_ok:
        sys.exit("synthesis check failed")
    if not (head.startswith("ff") or "49 44 33" in head):
        sys.exit("payload is not MP3 (head %s)" % head)

    ssh.close()
    print("=== done: %s on %s:5050" % (IMAGE, HOST))


if __name__ == "__main__":
    main()
