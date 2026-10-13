import os
import shlex
import subprocess
import sys
from pathlib import Path

import click

APPS = Path(__file__).resolve().parent / "apps" / "galois"
BUILD = Path(__file__).resolve().parent / "build" / "galois"
CMAKE_FLAGS = [
    "-DCMAKE_BUILD_TYPE=Release",
]


def execute(cmd: list[str], env: dict[str, str] | None = None) -> int:
    env = env or {}
    click.echo(shlex.join([*(f"{k}={v}" for k, v in env.items()), *cmd]), err=True)
    return subprocess.run(cmd, env={**os.environ, **env}).returncode


def mpirun(np_: int) -> list[str]:
    if np_ == 1:
        return ["mpirun", "-n", "1", "--bind-to", "none", "numactl", "-i", "all"]
    return ["mpirun", "-n", str(np_), "--map-by", "socket", "--bind-to", "socket"]


@click.group()
def main() -> None:
    """Build and run D-Galois apps under Open MPI."""


@main.command()
@click.argument("app")
def build(app: str) -> None:
    """Configure apps/galois, which builds external/Galois with distributed support, and
    build its CMake target APP into build/galois/APP."""
    code = execute(["cmake", "-S", str(APPS), "-B", str(BUILD), *CMAKE_FLAGS])
    if code:
        sys.exit(code)
    sys.exit(execute(["cmake", "--build", str(BUILD), "--target", app, "--parallel"]))


@main.command()
@click.argument("app")
@click.argument("graph", type=click.Path(exists=True, dir_okay=False))
@click.option("-n", "--np", "np_", type=int, default=1, help="Number of MPI processes.")
@click.option("-t", "--threads", type=int, default=1, help="Threads per process.")
@click.option("-r", "--rounds", type=int, default=3, help="Timed rounds after the warm-up.")
def run(app: str, graph: str, np_: int, threads: int, rounds: int) -> None:
    """Run APP on GRAPH, a symmetric graph in Galois's .gr format, under mpirun.

    APP prints load_s, then one untimed warm-up and a "round r seconds" line
    per timed round, then its result, on rank 0.

    -n 1 runs one unbound rank with memory interleaved across sockets
    (numactl -i all); larger -n binds one rank per socket.
    """
    binary = BUILD / app
    if not binary.exists():
        raise click.ClickException(f"{binary} not found; run `build {app}` first")
    cmd = [
        *mpirun(np_),
        str(binary),
        graph,
        "-symmetricGraph",
        "-t",
        str(threads),
        "-runs",
        str(rounds),
    ]
    sys.exit(execute(cmd, {"GALOIS_DO_NOT_BIND_THREADS": "1"}))


if __name__ == "__main__":
    main()
