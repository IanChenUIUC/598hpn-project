import os
import shlex
import subprocess
import sys
from pathlib import Path

import click

GRAPE = Path(__file__).resolve().parents[2] / "external" / "libgrape-lite"
BUILD = Path(__file__).resolve().parent / "build" / "grape"
CMAKE_FLAGS = [
    "-DCMAKE_BUILD_TYPE=Release",
]


def execute(cmd: list[str], env: dict[str, str] | None = None) -> int:
    env = env or {}
    click.echo(shlex.join([*(f"{k}={v}" for k, v in env.items()), *cmd]), err=True)
    return subprocess.run(cmd, env={**os.environ, **env}).returncode


@click.group()
def main() -> None:
    """Build and run libgrape-lite apps under Open MPI."""


@main.command()
def build() -> None:
    """Configure external/libgrape-lite and build run_app, which contains every app."""
    code = execute(["cmake", "-S", str(GRAPE), "-B", str(BUILD), *CMAKE_FLAGS])
    if code:
        sys.exit(code)
    sys.exit(execute(["cmake", "--build", str(BUILD), "--target", "analytical_apps", "--parallel"]))


@main.command()
@click.argument("app")
@click.argument("graph")
@click.option("-n", "--np", "np_", type=int, default=1, help="Number of MPI processes.")
@click.option("-t", "--threads", type=int, default=1, help="Threads per process.")
@click.option(
    "-o",
    "--out",
    type=click.Path(file_okay=False),
    help="Directory for per-fragment results; none if omitted.",
)
def run(app: str, graph: str, np_: int, threads: int, out: str | None) -> None:
    """Run APP (e.g. wcc, pagerank) on GRAPH.v and GRAPH.e, LDBC-format vertex and edge files."""
    vfile, efile = Path(f"{graph}.v"), Path(f"{graph}.e")
    for path in (vfile, efile):
        if not path.is_file():
            raise click.ClickException(f"{path} not found")
    binaries = [p for p in BUILD.rglob("run_app") if p.is_file()]
    if len(binaries) != 1:
        raise click.ClickException(
            f"expected one run_app under {BUILD}, found {len(binaries)}; run `build` first"
        )
    cmd = [
        "mpirun",
        "-n",
        str(np_),
        str(binaries[0]),
        "--application",
        app,
        "--vfile",
        str(vfile),
        "--efile",
        str(efile),
        "--app_concurrency",
        str(threads),
    ]
    if out:
        cmd += ["--out_prefix", out]
    sys.exit(execute(cmd))


if __name__ == "__main__":
    main()
