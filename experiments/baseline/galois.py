import os
import shlex
import subprocess
import sys
from pathlib import Path

import click

GALOIS = Path(__file__).resolve().parents[2] / "external" / "Galois"
BUILD = Path(__file__).resolve().parent / "build" / "galois"
CMAKE_FLAGS = [
    "-DCMAKE_BUILD_TYPE=Release",
    "-DGALOIS_ENABLE_DIST=ON",
]


def execute(cmd: list[str], env: dict[str, str] | None = None) -> int:
    env = env or {}
    click.echo(shlex.join([*(f"{k}={v}" for k, v in env.items()), *cmd]), err=True)
    return subprocess.run(cmd, env={**os.environ, **env}).returncode


@click.group()
def main() -> None:
    """Build and run D-Galois apps under Open MPI."""


@main.command()
@click.argument("app")
def build(app: str) -> None:
    """Configure external/Galois with distributed support and build the CMake target APP."""
    code = execute(["cmake", "-S", str(GALOIS), "-B", str(BUILD), *CMAKE_FLAGS])
    if code:
        sys.exit(code)
    sys.exit(execute(["cmake", "--build", str(BUILD), "--target", app, "--parallel"]))


@main.command()
@click.argument("app")
@click.argument("graph", type=click.Path(exists=True, dir_okay=False))
@click.option("-n", "--np", "np_", type=int, default=1, help="Number of MPI processes.")
@click.option("-t", "--threads", type=int, default=1, help="Threads per process.")
def run(app: str, graph: str, np_: int, threads: int) -> None:
    """Run APP on GRAPH, a symmetric graph in Galois's .gr format, under mpirun.

    connected-components-push-dist prints "Number of components is N" on rank 0.
    """
    binaries = [p for p in BUILD.rglob(app) if p.is_file()]
    if len(binaries) != 1:
        raise click.ClickException(
            f"expected one {app} under {BUILD}, found {len(binaries)}; run `build {app}` first"
        )
    cmd = ["mpirun", "-n", str(np_), str(binaries[0]), graph, "-symmetricGraph", "-t", str(threads)]
    sys.exit(execute(cmd, {"GALOIS_DO_NOT_BIND_THREADS": "1"}))


if __name__ == "__main__":
    main()
