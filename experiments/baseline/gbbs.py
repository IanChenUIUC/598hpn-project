import shlex
import subprocess
import sys
from pathlib import Path

import click

EXTERNAL = Path(__file__).resolve().parents[2] / "external"
GBBS = EXTERNAL / "gbbs"
PARLAY = EXTERNAL / "parlaylib" / "include"
APPS = Path(__file__).resolve().parent / "apps" / "gbbs"
BUILD = Path(__file__).resolve().parent / "build" / "gbbs"
CXXFLAGS = [
    "-std=c++17",
    "-march=native",
    "-mcx16",
    "-O3",
    "-DNDEBUG",
    "-DGBBSLONG",
    "-DAMORTIZEDPD",
    "-DUSEMALLOC",
    "-pthread",
]
SOURCES = [
    "gbbs/graph_io.cc",
    "gbbs/io.cc",
    "gbbs/encodings/byte.cc",
    "gbbs/encodings/byte_pd.cc",
    "gbbs/encodings/byte_pd_amortized.cc",
    "gbbs/helpers/parse_command_line.cc",
    "benchmarks/Connectivity/common.cc",
    "benchmarks/Connectivity/connectit.cc",
]


def execute(cmd: list[str]) -> int:
    click.echo(shlex.join(cmd), err=True)
    return subprocess.run(cmd).returncode


@click.group()
def main() -> None:
    """Build and run GBBS apps on a single node."""


@main.command()
@click.argument("app")
def build(app: str) -> None:
    """Compile apps/gbbs/APP.cc against external/gbbs and external/parlaylib into
    build/gbbs/APP, with GBBS's default scheduler."""
    BUILD.mkdir(parents=True, exist_ok=True)
    source = APPS / f"{app}.cc"
    cmd = [
        "g++",
        *CXXFLAGS,
        "-I",
        str(GBBS),
        "-I",
        str(PARLAY),
        str(source),
        *(str(GBBS / s) for s in SOURCES),
        "-o",
        str(BUILD / app),
    ]
    sys.exit(execute(cmd))


@main.command()
@click.argument("app")
@click.argument("graph")
@click.option("-r", "--rounds", type=int, default=3, help="Timed rounds after the warm-up.")
def run(app: str, graph: str, rounds: int) -> None:
    """Run APP on GRAPH, a symmetric graph in GBBS's binary format.

    APP prints load_s, then one untimed warm-up and a "round r seconds" line
    per timed round, then its result. Runs with memory interleaved across sockets
    (numactl -i all).
    """
    if not Path(graph).is_file():
        raise click.ClickException(f"{graph} not found")
    binary = BUILD / app
    if not binary.exists():
        raise click.ClickException(f"{binary} not found; run `build {app}` first")
    sys.exit(execute(["numactl", "-i", "all", str(binary), "-rounds", str(rounds), graph]))


if __name__ == "__main__":
    main()
