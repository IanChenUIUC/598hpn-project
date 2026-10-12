import shlex
import subprocess
import sys
from pathlib import Path

import click

LIGRA = Path(__file__).resolve().parents[2] / "external" / "ligra"
APPS = Path(__file__).resolve().parent / "apps" / "ligra"
BUILD = Path(__file__).resolve().parent / "build" / "ligra"
CXXFLAGS = [
    "-std=c++14",
    "-fopenmp",
    "-march=native",
    "-O3",
    "-DOPENMP",
    "-DBYTERLE",
    "-DLONG",
    "-include",
    "cstdint",
]


def execute(cmd: list[str]) -> int:
    click.echo(shlex.join(cmd), err=True)
    return subprocess.run(cmd).returncode


@click.group()
def main() -> None:
    """Build and run Ligra apps on a single node."""


@main.command()
@click.argument("app")
def build(app: str) -> None:
    """Compile apps/ligra/APP.C against external/ligra into build/ligra/APP."""
    BUILD.mkdir(parents=True, exist_ok=True)
    source = APPS / f"{app}.C"
    cmd = ["g++", *CXXFLAGS, "-I", str(LIGRA / "ligra"), str(source), "-o", str(BUILD / app)]
    sys.exit(execute(cmd))


@main.command()
@click.argument("app")
@click.argument("graph")
@click.option("-r", "--rounds", type=int, default=3, help="Timed rounds after the warm-up.")
def run(app: str, graph: str, rounds: int) -> None:
    """Run APP on GRAPH, the prefix of a symmetric graph in Ligra's binary format
    (GRAPH.config, GRAPH.adj, GRAPH.idx).

    APP prints load_s, then one untimed warm-up and a "round r seconds" line
    per timed round, then its result.
    """
    if not Path(f"{graph}.config").is_file():
        raise click.ClickException(f"{graph}.config not found")
    binary = BUILD / app
    if not binary.exists():
        raise click.ClickException(f"{binary} not found; run `build {app}` first")
    sys.exit(execute([str(binary), "-rounds", str(rounds), graph]))


if __name__ == "__main__":
    main()
