import json
import os
import shlex
import socket
import subprocess
import sys
from pathlib import Path

PORTAL_HTTP = "https://boss.emulab.net:43794"
TOKEN = Path("/proj/ECE509/ianchen3-cloudlabtoken.jwt")
PROJECT = "ece509"
CLI = ["uvx", "--from", "portal-api[cli] @ git+https://gitlab.flux.utah.edu/emulab/portal-api",
       "portal-cli"]


def portal(*args: str) -> str:
    cmd = [*CLI, *args]
    print(shlex.join(cmd), file=sys.stderr)
    env = {**os.environ, "PORTAL_HTTP": PORTAL_HTTP, "PORTAL_TOKEN": TOKEN.read_text().strip()}
    return subprocess.run(cmd, env=env, check=True, stdout=subprocess.PIPE, text=True).stdout


def main() -> None:
    name = socket.getfqdn().split(".")[1]
    experiments = json.loads(portal("experiment", "list"))["experiments"]
    ids = [e["id"] for e in experiments if e["name"] == name and e["project"] == PROJECT]
    if len(ids) != 1:
        sys.exit(f"expected one experiment named {name} in {PROJECT}, found {ids}")
    portal("experiment", "terminate", "--experiment-id", ids[0])


if __name__ == "__main__":
    main()
